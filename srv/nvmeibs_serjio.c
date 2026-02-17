/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * manages disk locks fuctionality
 *
 * 5/2015
 */

#include "nvmeibs_serjio.h"
#include "nvmeib_shared.h"
#include "nvmeib_trace_warns.h"
#include "nvmeibs_serjio_deps.h"
#include "nvmeibs_serjio_gpt.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_defs.h"
#include "nvmeib_utils.h"
#include "nvmeib_types.h"
#include "nvmeib_public.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_utils.h"
#include "nvmeibs_toma.h"
#include "nvmeibs_trace.h"
#include "nvmeibs_um_comm.h"
#include "clnt/block/datapath_utils_generic/nvmeibc_block_dp_defs.h"	// NVMEIBC_SECTOR_SIZE, LOCKSET_4KS, ...
#include "common/proc_epilog.h"
#include "nvmeibs_memmgr_metrics.h"

#if defined(BLKDEV_SIMULATOR)
	#include "clnt/block/unitest/nvmeibc_simu_disk.h"
#endif
#ifdef __KERNEL__
	#include "common/compat/kr_incs_crc32.inc.c"
#endif

#if defined(BLKDEV_SIMULATOR)
static bool invalid_jris_exist_on_disk = true;
#else
static bool invalid_jris_exist_on_disk = false;
#endif

static bool stamp_jrnl_entries = true;
module_param_named(stamp_free_jrnl_entries, stamp_jrnl_entries, bool, 0444);
MODULE_PARM_DESC(stamp_free_jrnl_entries, "Stamp free journal entries for debugging purposes.");

static unsigned nvmeibs_jrange_num_blocks = (1 << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT);
module_param_named(nvmeibs_jrange_num_blocks, nvmeibs_jrange_num_blocks, uint, 0444);
MODULE_PARM_DESC(nvmeibs_jrange_num_blocks, "Total number of journal blocks in journal range, typically allocated to a single client. Should be set to a power of 2, between 64 and 16384.");

static unsigned nvmeibs_serjio_resched_work_wait_max = 10;
module_param_named(serjio_resched_work_wait_max, nvmeibs_serjio_resched_work_wait_max, uint, 0644);
MODULE_PARM_DESC(serjio_resched_work_wait_max, "Amount of time in seconds to wait for a rescheduled SERJIO work item to run. SERJIO work items are related to garbage collection and cleaning up of journal entries.");

static unsigned nvmeibs_serjio_jgc_avail_ent_low_wm_mult = NVMEIB_JGC_AVAIL_ENT_LOW_WM_MULT;
module_param_named(jgc_avail_ent_low_wm_mult, nvmeibs_serjio_jgc_avail_ent_low_wm_mult, uint, 0644);
MODULE_PARM_DESC(jgc_avail_ent_low_wm_mult, "Triggers JGC (journal garbage collection) if the available range of entries falls below the low watermark of total-range-entries * mult / div.");

static unsigned nvmeibs_serjio_jgc_avail_ent_low_wm_div = NVMEIB_JGC_AVAIL_ENT_LOW_WM_DIV;
module_param_named(jgc_avail_ent_low_wm_div, nvmeibs_serjio_jgc_avail_ent_low_wm_div, uint, 0644);
MODULE_PARM_DESC(jgc_avail_ent_low_wm_div, "Triggers JGC (journal garbage collection) if the available range of entries falls below the low watermark of total-range-entries * mult / div.");

static unsigned nvmeibs_serjio_init_db_interrupt_range = ~(unsigned)0;
static bool nvmeibs_serjio_fail_next_gpt_update = false;
static bool nvmeibs_serjio_fail_next_gpt_init = false;
static bool next_free_alloc_quarantined = false;
static int next_free_alloc_quarantined_idx = -1;

#if !defined(NVMESH_IS_PRODUCTION_COMPILATION) || (NVMESH_IS_PRODUCTION_COMPILATION==0)

module_param_named(serjio_init_db_interrupt_range, nvmeibs_serjio_init_db_interrupt_range, uint, 0644);
MODULE_PARM_DESC(serjio_init_db_interrupt_range, "Interrupt Init DB when it gets to this range.");

module_param_named(serjio_fail_next_gpt_update, nvmeibs_serjio_fail_next_gpt_update, bool, 0644);
MODULE_PARM_DESC(serjio_fail_next_gpt_update, "SERJIO - Fail the next GPT update (for testing).");

module_param_named(serjio_fail_next_gpt_init, nvmeibs_serjio_fail_next_gpt_init, bool, 0644);
MODULE_PARM_DESC(serjio_fail_next_gpt_init, "SERJIO - Fail the next GPT init (for testing).");


module_param_named(serjio_invalid_jris_exist_on_disk, invalid_jris_exist_on_disk, bool, 0444);
MODULE_PARM_DESC(serjio_invalid_jris_exist_on_disk, "Allocate (but quarantine) invalid JRIs (0,1,2) on disk.");

module_param_named(serjio_next_free_alloc_quarantined, next_free_alloc_quarantined, bool, 0644);
MODULE_PARM_DESC(serjio_next_free_alloc_quarantined, "SERJIO - Allocate a quarantined range index for the next allocation (for testing).");

module_param_named(serjio_next_free_alloc_quarantined_idx, next_free_alloc_quarantined_idx, int, 0644);
MODULE_PARM_DESC(serjio_next_free_alloc_quarantined_idx, "Quarantined range index for next invalid allocation.");
#endif

#define SERJIO_WQ_PEND_MAX_WAIT (nvmeibs_serjio_resched_work_wait_max * HZ)

#undef SERJIO_DEBUG_GPT
#undef SERJIO_NVME_OP_RSRC_STATE_CALL_STACK

/* Code for SERJIO - The Server Journal Manager */
/*==============================================*/
/* GLOSSARY OF TERMS:				*/
/* ---------------------------------------------*/
/* JMDC - Journal Metadata Cache 		*/

#define PRIMARY_GPT_HEADER_LBA	1
#define GPT_HEADER_SIZE_LBA		1

#define CLEAN_ENTRIES_WITH_INVALID_J2D 1

#define ALLOC_JMDC_AFTER_RD_GPT		1

#define DISK_LBA_CLSECT_SHIFT(  di) (NVMEIBC_SECTOR_SHIFT                   - nvmeibs_disk_info_get_block_shift(di))
#define DISK_LBA_JRNL_ENT_SHIFT(di, binje_shift) (NVMEIB_EC_JOURNAL_SECTOR_SHIFT + binje_shift - nvmeibs_disk_info_get_block_shift(di))
#define DISK_LBA_DB_ENT_SHIFT(  di) (NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT - nvmeibs_disk_info_get_block_shift(di))

#define DISK_LBAS_TO_JOURNAL_ENTS(  di, binje_shift, lbas) ((lbas) >> (DISK_LBA_JRNL_ENT_SHIFT(di, binje_shift)))
#define DISK_LBAS_TO_JOURNAL_BLKS(  di, lbas) ((lbas) >> (DISK_LBA_JRNL_ENT_SHIFT(di, 0)))
#define DISK_LBAS_TO_SERJIO_DB_ENTS(di, lbas) ((lbas) >> DISK_LBA_DB_ENT_SHIFT(di))
#define DISK_LBAS_TO_NVMEIBC_SECTS( di, lbas) ((lbas) >> DISK_LBA_CLSECT_SHIFT(di))
#define DISK_LBAS_TO_BYTES(         di, lbas) ((lbas) << nvmeibs_disk_info_get_block_shift(di))

#define JOURNAL_ENTS_TO_DISK_LBAS(di, binje_shift, ents) ((ents) << DISK_LBA_JRNL_ENT_SHIFT(di, binje_shift))
#define JOURNAL_BLKS_TO_DISK_LBAS(di, blks) 		JOURNAL_ENTS_TO_DISK_LBAS(di, 0, blks)
#define SERJIO_DB_ENTS_TO_DISK_LBAS(di, ents    ) ((ents    ) << DISK_LBA_DB_ENT_SHIFT(di))
#define NVMEIBC_SECTS_TO_DISK_LBAS( di, nv_sects) ((nv_sects) << DISK_LBA_CLSECT_SHIFT(di))
#define JOURNAL_ENTS_TO_NVMEIBC_SECTS(binje_shift, ents)    ((ents) << (binje_shift + NVMEIB_EC_JOURNAL_SECTOR_SHIFT - NVMEIBC_SECTOR_SHIFT))

#define JOURNAL_ENTS_TO_BYTES(di, binje_shift, ents)			(DISK_LBAS_TO_BYTES((di), JOURNAL_ENTS_TO_DISK_LBAS((di), (binje_shift), (ents))))

#define SERJIO_DB_ENTS_TO_BYTES(di, ents)			(DISK_LBAS_TO_BYTES((di), SERJIO_DB_ENTS_TO_DISK_LBAS((di), (ents))))
#define SERJIO_DB_ENTS_TO_8B(di, ents)				((DISK_LBAS_TO_BYTES((di), SERJIO_DB_ENTS_TO_DISK_LBAS((di), (ents)))) >> 3)

#if NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE < BITS_PER_LONG
	// Currently, simulaltor code only. Hopefully will never have less entries than bits per long
	#define INCREMENT_ENTRY_BITMAP(bmap) ((bmap) = (void*)((u8*)(bmap) + (NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE/BITS_PER_BYTE)))
#else
	#define INCREMENT_ENTRY_BITMAP(bmap) ((bmap) +=  BITS_TO_LONGS(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE))
#endif

#define JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, range_idx, entry) \
	(DISK_LBAS_TO_NVMEIBC_SECTS(serjio_pd->di, (serjio_pd->disk_ranges.journal.lba + serjio_pd->jranges_alloc_tbl.ranges[range_idx].rng_rlba)) + \
			JOURNAL_ENTS_TO_NVMEIBC_SECTS(serjio_pd->jranges_alloc_tbl.ranges[range_idx].binje_shift, entry))

#define SERJIO_DB_ENT_TO_NVMEIBC_SECT(serjio_pd, entry) \
	(DISK_LBAS_TO_NVMEIBC_SECTS(serjio_pd->di, serjio_pd->disk_ranges.db.lba + \
		SERJIO_DB_ENTS_TO_DISK_LBAS(serjio_pd->di, entry)))

#define _NTs(name, _pd, fmt, ...) \
	_NT(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)

#define _NTs_short(name, _pd, fmt, ...) \
	NVMEIB_LOG_LONGTERM("SERJIO (@SERJIO_PD): " fmt, _T, /*Default scope*/, name, _pd, ## __VA_ARGS__)

#define _NIs(name, _pd, fmt, ...) \
	_NI(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)

#define _NWs(name, _pd, fmt, ...) \
	_NW(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)

#define _NEs(name, _pd, fmt, ...) \
	_NE(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)
	
#define _NEs_dmesg(name, _pd, fmt, ...) \
	_NE_dmesg(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)

#define _NDs(name, _pd, fmt, ...) \
	_ND(name, "SERJIO (@SERJIO_PD): Disk @DISK_ID_STR (@DISK): " fmt, \
		_pd, nvmeibs_disk_info_get_disk_id(_pd->di), _pd->di, ## __VA_ARGS__)

#define SERJIO_BUG_ON(cond, name, _pd, fmt, ...) \
	do {\
		if (unlikely((cond))) {\
			_NEs(name, _pd, fmt, ## __VA_ARGS__);\
			printk(KERN_ALERT "SERJIO BUG: \"" #cond "\" failed at %s:%d/%s()!\n",\
					__FILE__, __LINE__, __func__);\
			asm volatile("");\
			BUG();\
		}\
	} while(0)

NVMEIBS_MEMMGR_METRIC(serjio_entry_table, "component=target.disks.serjio.entry_table");
NVMEIBS_MEMMGR_METRIC(serjio_jmdc_mem, "component=target.disks.serjio.jmdc.mem");
NVMEIBS_MEMMGR_METRIC(serjio_jmdc_map, "component=target.disks.serjio.jmdc.map");

/*************************** Added for SERJIO *********************************/
enum nvmeib_shared_jentry_md_chain_error {
	NVMEIB_JENTRY_CHAIN_OK = 0,
	NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0,
	/* NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_1 ... NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_14 */
	NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_MAX = NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0 + NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY - 1,
	NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0,
	/* NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0 ... NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_14 */
	NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_MAX = NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0 + NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY - 1,
	NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0,
	/* NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0 ... NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_14 */
	NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_MAX = NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0 + NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY - 1,
	NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0,
	/* NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0 ... NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_14 */
	NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_MAX = NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0 + NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY - 1,
	NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0,
	/* NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0 ... NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_14 */
	NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_MAX = NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0 + NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY - 1,
};

static inline const char *nvmeib_shared_jentry_md_chain_err_str(enum nvmeib_shared_jentry_md_chain_error chain_err)
{
	switch (chain_err) {
		case NVMEIB_JENTRY_CHAIN_OK:
			return "CHAIN_OK";
		case NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0 ... NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_MAX:
			return "LAST_HAS_NEXT";
		case NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0 ... NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_MAX:
			return "BLOCK_NOT_IO";
		case NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0 ... NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_MAX:
			return "BLOCK_INVALID_VERSION";
		case NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0 ... NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_MAX:
			return "BLOCK_J2D_NOT_CONSECUTIVE";
		case NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0 ... NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_MAX:
			return "BLOCK_TXID_MISMATCH";
		default:
			break;
	}
	return "UNKNOWN_ERROR";
}

static inline int nvmeib_shared_jentry_md_chain_err_block_idx(enum nvmeib_shared_jentry_md_chain_error chain_err)
{
	switch (chain_err) {
		case NVMEIB_JENTRY_CHAIN_OK:
			return 0;
		case NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0 ... NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_MAX:
			return chain_err - NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0;
		case NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0 ... NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_MAX:
			return chain_err - NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0;
		case NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0 ... NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_MAX:
			return chain_err - NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0;
		case NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0 ... NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_MAX:
			return chain_err - NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0;
		case NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0 ... NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_MAX:
			return chain_err - NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0;
		default:
			break;
	}
	return -1;
}

int nvmeibs_serjio_jmd_decode_j2d_chain(const union jblock_md *jentry, const binje_t binje, u64 *j2d_start, u64 *j2d_end)
{
	const union jblock_md *first = jentry, *cur = jentry;
	unsigned jb = 0;
	if (jentry->version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED)	{
		// Does not support chain,
		if (!nvmeib_is_jmd_io_entry(*cur)) {
			_NW(warn_serjio_jmd_decode_j2d_chain_not_io_unpacked,
			    "JBlock[@JRNL_ENT_BLOCK_IDX], not IO entry, raw @JMDC_ENT", jb, cur->raw);
			WARN_ON(1);
			return NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0;
		}
		goto chain_ok;
	}
	for (;;) {
		if (!nvmeib_is_jmd_io_entry(*cur)) {
			_NW(warn_serjio_jmd_decode_j2d_chain_not_io,
			    "JBlock[@JRNL_ENT_BLOCK_IDX], not IO entry, raw @JMDC_ENT", jb, cur->raw);
			WARN_ON(1);
			return NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0 + jb;
		}
		if (cur->version != first->version) {
			_NW(warn_serjio_jmd_decode_j2d_chain_ver_mismatch,
				"jblock[@JRNL_ENT_BLOCK_IDX]: version @VERSION, raw @JMDC_ENT, does not match first version @VERSION, raw @JMDC_ENT",
				jb, cur->version, cur->raw, first->version, first->raw);
			WARN_ON(1);
			return NVMEIB_JENTRY_CHAIN_BLOCK_INVALID_VERSION_0 + jb;								// Version changed, this is definitely not a chain.
		}												// Versions match, safe to test other fields
		if (cur->j2d_0 != (first->j2d_0+jb) || (cur->tx_id != first->tx_id)) {
			// J2D and TxID dont match, Probably IO was aborted during chain write
			_NW(warn_serjio_jmd_decode_j2d_chain_txid_mismatch,
			    "jblock[@JRNL_ENT_BLOCK_IDX]:j2d @J2D, tx_id @TXID, raw @JMDC_ENT, does not match first: j2d @J2D, tx_id @TXID, raw @JMDC_ENT",
				jb, cur->j2d_0, cur->tx_id, cur->raw, first->j2d_0, first->tx_id, first->raw);
#ifndef BLKDEV_SIMULATOR
			WARN_ON(1);
#endif
			return cur->tx_id == first->tx_id ? NVMEIB_JENTRY_CHAIN_J2D_NOT_CONSECTIVE_0 + jb : NVMEIB_JENTRY_CHAIN_TXID_MISMATCH_0 + jb;
		}
		if (++jb == binje) {
			if (cur->v1.has_next) {
				_NW(warn_serjio_jmd_decode_j2d_chain_last_has_next,
				    "jblock[@JRNL_ENT_BLOCK_IDX]: last has next, raw @JMDC_ENT", jb, cur->raw);
				WARN_ON(1);
				return NVMEIB_JENTRY_CHAIN_LAST_HAS_NEXT_0 + jb;
			}
			break;
		}
		if (!cur->v1.has_next)
			break;
		cur++;
	}
chain_ok:
	if (j2d_start)
		*j2d_start = nvmeibc_block_dp_ec_jmd_decode_j2d_only(first);
	if (j2d_end)
		*j2d_end = nvmeibc_block_dp_ec_jmd_decode_j2d_only(cur);
	return NVMEIB_JENTRY_CHAIN_OK;
}

/******************** Dependency functions for simulators *****************/
enum jentry_state_mask {
	JENTRY_UNKNOWN_MASK = (1 << JENTRY_UNKNOWN),
	JENTRY_SYNCED_MASK = (1 << JENTRY_SYNCED),
	JENTRY_FREE_MASK = (1 << JENTRY_FREE),
	JENTRY_TAKEN_MASK = (1 << JENTRY_TAKEN),
	JENTRY_ABND_MASK = (1 << JENTRY_ABND),
	JENTRY_WAIT_RET_MASK = (1 << JENTRY_WAIT_RET),
	JENTRY_ALL_MASK = ((1 << MAX_JENTRY_STATE) - 1),
	JENTRY_DIRTY_MASK = (JENTRY_UNKNOWN_MASK | JENTRY_SYNCED_MASK | JENTRY_ABND_MASK), /* Entries that are dirty and need recovery */
	JENTRY_OWN_JAM_MASK = (JENTRY_TAKEN_MASK | JENTRY_WAIT_RET_MASK), /* Entries that are owned by JAM */
	JENTRY_OWN_SRJ_MASK = (JENTRY_ALL_MASK & ~JENTRY_OWN_JAM_MASK), /* Entries that are owned by SERJIO */
	JENTRY_UNSYNCED_MASK = (JENTRY_UNKNOWN_MASK | JENTRY_TAKEN_MASK | JENTRY_WAIT_RET_MASK) /* Entries where JMDC may not equal JMDD */
};

#define NUM_BITS_JENTRY_STATE		(4)

__attribute__((unused)) static void build_checks(void) {
	BUILD_BUG_ON(MAX_JENTRY_STATE > (1 << NUM_BITS_JENTRY_STATE));
}
#define JENTRY_STATE_MASK			((1 << NUM_BITS_JENTRY_STATE) - 1)

#define NUM_JENTRY_STATES_PER_LONG 	((BITS_PER_LONG) / NUM_BITS_JENTRY_STATE)

#define NUM_JENTRY_STATE_LONGS		((NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE + NUM_JENTRY_STATES_PER_LONG - 1) / NUM_JENTRY_STATES_PER_LONG)

#define JENTRY_STATE_LONG_IN_BMP(bmp, entry) \
	bmp[entry / NUM_JENTRY_STATES_PER_LONG]

#define JENTRY_STATE_IN_LONG_SHIFT(entry) \
	(NUM_BITS_JENTRY_STATE * (entry % NUM_JENTRY_STATES_PER_LONG))

#define JENTRY_STATE_IN_LONG_MASK(entry) \
	((unsigned long)JENTRY_STATE_MASK << JENTRY_STATE_IN_LONG_SHIFT(entry))

#define GET_JENTRY_STATE_FROM_BMP(bmp, entry) \
	((JENTRY_STATE_LONG_IN_BMP(bmp, entry) >> \
	JENTRY_STATE_IN_LONG_SHIFT(entry)) & JENTRY_STATE_MASK)

#define SET_JENTRY_STATE_IN_BMP(bmp, entry, state) \
	do {\
		JENTRY_STATE_LONG_IN_BMP(bmp, entry) &= ~JENTRY_STATE_IN_LONG_MASK(entry); \
		JENTRY_STATE_LONG_IN_BMP(bmp, entry) |= \
			((unsigned long)(state & JENTRY_STATE_MASK) << JENTRY_STATE_IN_LONG_SHIFT(entry)); \
	} while(0)

#define JENTRY_STATE_IN_MASK(state, mask)	((1 << state) & mask)

#define JENTRY_DIRTY(state)				JENTRY_STATE_IN_MASK(state, JENTRY_DIRTY_MASK)
#define JENTRY_UNSYNCED(state)			JENTRY_STATE_IN_MASK(state, JENTRY_UNSYNCED_MASK)
#define JENTRY_FOR_JGC(state)			JENTRY_STATE_IN_MASK(state, JENTRY_JGC_MASK)
#define JENTRY_OWNED_BY_JAM(state)		JENTRY_STATE_IN_MASK(state, JENTRY_OWN_JAM_MASK)
#define JENTRY_OWNED_BY_SERJIO(state)	JENTRY_STATE_IN_MASK(state, JENTRY_OWN_SRJ_MASK)

/* SERJIO DB contains an entry pe journal range. It is stored on the disk, one sw sector per journal range */
#define SERJIO_DB_JRANGE_ENTRY_MAGIC (*(__be64*)"SERJIODB")
#define SERJIO_DB_CURR_VERSION_MAJOR 1
#define SERJIO_DB_CURR_VERSION_MINOR 4

#define SERJIO_DB_VERSION_CODE(maj, min) (((u16)(maj) << 8) | (min))

#define SERJIO_DB_VERSION_1_1_CODE		SERJIO_DB_VERSION_CODE(1, 1)
#define SERJIO_DB_VERSION_1_2_CODE		SERJIO_DB_VERSION_CODE(1, 2)
#define SERJIO_DB_VERSION_1_3_CODE		SERJIO_DB_VERSION_CODE(1, 3)
#define SERJIO_DB_VERSION_1_4_CODE		SERJIO_DB_VERSION_CODE(1, 4)

#define SERJIO_DB_MIN_VERSION_CODE		SERJIO_DB_VERSION_1_1_CODE
#define SERJIO_DB_MAX_VERSION_CODE		SERJIO_DB_VERSION_1_4_CODE


#define JRANGE_INVALID_BINJE_SHIFT		(U8_MAX)

struct serjio_db_jrange_entry_hdr
{
	__be64 magic;
	u8 version_major;
	u8 version_minor;
	__be16 len_8b;
	__be32 crc32;
} __attribute__((packed));

/* TBD: Use VEX for this */
struct serjio_db_jrange_entry_data
{
	/* VERSION 1.1 below */
	__be32 range_idx;
	__be32 range_status;
	__be64 last_client_id;
	uuid_be client_uuid;
	char client_host[NVMEIB_HOST_NAME_LEN];
	__be64 reserved_secs;
	__be64 last_allocated_secs;
	__be64 last_returned_secs;
	__be64 gen_id;
	/* VERSION 1.2 below */
	u8 binje_shift;
	u8 padding12[3];
	/* VERSION 1.3 below */
	__be32 rng_rlba;
	__be32 rng_nlba;
	__be32 rng_rblk;
	__be32 rng_nblk;
	/* VERSION 1.4 below */
	uuid_be init_db_uuid;		/* This is set to a new uuid every time the DB is initialised */
	uuid_be init_db_uuid_done;	/* After init is completed, this is set to the init_db_uuid value for the first and last entries ONLY */
}  __attribute__((packed));

struct serjio_db_jrange_entry
{
	union {
		struct {
			struct serjio_db_jrange_entry_hdr hdr;
			struct serjio_db_jrange_entry_data data;
		};
		u8 bytes[1 << NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT];
	};
} __attribute__((packed));

struct cln_jrnl_disk_rng_param {
	u64 start_lba;
	u64 end_lba;
	const char *seg_uuid_str;
	bool seg_delete;
};

struct jrange_entry {
	struct nvmeibs_serjio_disk_private_data *serjio_pd;

	//this is actually serjio_db_jrange_entry fields,
	u32 range_idx;
	enum nvmeibs_serjio_jrange_status status;
	u64 client_id;
	uuid_be client_uuid;
	char client_host[NVMEIB_HOST_NAME_LEN];
	struct timespec reserved;
	struct timespec last_allocated;
	struct timespec last_returned;
	u64 gen_id;
	u8 binje_shift;
	u16 n_ents;
	u32 rng_rlba;
	u32 rng_nlba;
	u32 rng_rblk;
	u32 rng_nblk;
	uuid_be init_db_uuid;
	uuid_be init_db_uuid_done;
	//end of serjio_db_jrange_entry fields
	spinlock_t lock;
	unsigned long jentry_state_bmp[NUM_JENTRY_STATE_LONGS];
	u16 jentry_state_cnt[MAX_JENTRY_STATE];
	struct nvmeib_jrnl_ent_md jentry_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	u64 returned_jif;
	bool ent_gen_id_wrapped;

	struct hlist_node link;

	/* Reference count while journal range is allocated */
	struct nvmeib_ref alloc_ref;
};
#define JMDC_MAP_PER_CLIENT		0

struct jranges_allocation_table {
	spinlock_t lock;
	DECLARE_HASHTABLE(reserved_ranges, 8); /* Use a hashtable so we can lookup by client_uuid */
	/* These don't need lookup but to reuse the hlist_node we use a hlist */
	struct hlist_head free_ranges, db_err_ranges, invalid_ranges, quarantined_ranges;
	struct jrange_entry *ranges;
	u32 num_ranges;
	u32 num_free_rngs;
	u32 num_valid_rngs;
	u32 num_reserved_ranges;
	u32 num_db_zero_rngs;
	u32 num_db_err_rngs;
	u32 num_quarantined_rngs;
#if JMDC_MAP_PER_CLIENT
	struct list_head jmdc_mappings;
#endif
};

struct nvmeibs_serjio_disk_private_data;
struct jmdc_mapping {
	struct list_head link;
	struct nvmeibs_dev *nic_dev;
	struct nvmeib_alloc_n_map jmdc_area_map;
	struct nvmeib_ref refcount;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	unsigned int sg_linear_len; /* If non-zero, each entry of the mapped sg has the same length (Used for jmdc sync) */
};

/* Resources for performing NVME operations */

#ifndef CONFIG_NVMEIB_DEBUG
#	define NUM_OF_NVME_OP_RSRC	(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE)
#else
#	define NUM_OF_NVME_OP_RSRC	1
#endif

#define NUM_GET_NVME_RSRC_ATTEMPTS	16

enum nvme_op_state
{
	NVME_OP_FREE = 0,
	NVME_OP_POSTED,
	NVME_OP_CB,
	NVME_OP_ERROR,
};

#define NVME_OP_RSRC_MAX_STACK_TRACE 3

struct nvme_op_rsrc
{
	struct nvmeibs_nvme_req nvme_req;

	/* Pages for Data + MD */
	void *virt;
	unsigned n_data_pgs;
	unsigned n_md_pgs;
	unsigned n_pages;

	/* Physical addresses of pages (Used for PRPL) */
	dma_addr_t *nvme_phys_virt;
	dma_addr_t nvme_phys_dma;
	size_t nvme_phys_size;

	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	unsigned range_idx;
	unsigned entry;
	unsigned binje_shift; /* blocks in journal entry */
	struct list_head link;
	struct completion *comp;
	atomic_t *comp_ctr;
	void *param;
	int status;

	atomic_t state;

#if defined(SERJIO_NVME_OP_RSRC_STATE_CALL_STACK) && (!defined(BLKDEV_SIMULATOR) || !BLKDEV_SIMULATOR)
	unsigned long state_chng_trace_ents[NVME_OP_RSRC_MAX_STACK_TRACE];
	struct nvmeib_stack_trace state_chng_stack_trace;
#endif

	struct nvmeibs_serjio_op_rsrc_stats op_rsrc_stats;

	nvme_callback_t *serjio_cb;
};

struct nvme_op_rsrc_pool {
	struct nvme_op_rsrc pool[NUM_OF_NVME_OP_RSRC];
	struct list_head free_list;
	spinlock_t lock;
	unsigned n_free;
	unsigned n_waiters;
	struct completion free_comp;
};

#define SERJIO_STATE_MASK(state) (1 << state)

#define SERJIO_STATE_BOOT_MASK() (\
	SERJIO_STATE_MASK(SERJIO_INIT) | \
	SERJIO_STATE_MASK(SERJIO_RD_GPT) | \
	SERJIO_STATE_MASK(SERJIO_RD_DB) | \
	SERJIO_STATE_MASK(SERJIO_RD_JRNL) | \
	SERJIO_STATE_MASK(SERJIO_INIT_JRNL) | \
	SERJIO_STATE_MASK(SERJIO_GPT_INIT) \
)

#define SERJIO_STATE_RUN_MASK() (\
	SERJIO_STATE_MASK(SERJIO_READY) | \
	SERJIO_STATE_MASK(SERJIO_CLN_JRNL) | \
	SERJIO_STATE_MASK(SERJIO_GPT_UPDATE) \
)

#define SERJIO_STATE_ERROR_MASK() (\
	SERJIO_STATE_MASK(SERJIO_ERR_GENERAL) | \
	SERJIO_STATE_MASK(SERJIO_ERR_GPT) | \
	SERJIO_STATE_MASK(SERJIO_ERR_NO_JRNL) | \
	SERJIO_STATE_MASK(SERJIO_ERR_NO_DB) | \
	SERJIO_STATE_MASK(SERJIO_ERR_RD_DB) | \
	SERJIO_STATE_MASK(SERJIO_ERR_WR_DB) | \
	SERJIO_STATE_MASK(SERJIO_ERR_RD_JRNL) | \
	SERJIO_STATE_MASK(SERJIO_ERR_WR_JRNL)\
)

#define SERJIO_STATE_HALT_MASK() (\
	SERJIO_STATE_ERROR_MASK() | \
	SERJIO_STATE_MASK(SERJIO_DYING)\
)

#define SERJIO_BOOTING(state) (SERJIO_STATE_MASK(state) & SERJIO_STATE_BOOT_MASK())
#define SERJIO_RUNNING(state) (SERJIO_STATE_MASK(state) & SERJIO_STATE_RUN_MASK())
#define SERJIO_HALTING(state) (SERJIO_STATE_MASK(state) & SERJIO_STATE_HALT_MASK())
#define SERJIO_ERROR(state) (SERJIO_STATE_MASK(state) & SERJIO_STATE_ERROR_MASK())


/* GPT Update State Machine */
enum serjio_gpt_upd_state {
	SERJIO_GPT_UPDATE_IDLE = 0,
	SERJIO_CHECK_GPT_PRE_WRITE,
	SERJIO_GPT_PRE_WRITE_DONE,
	SERJIO_CHECK_GPT_POST_WRITE,
	SERJIO_GPT_POST_WRITE_DONE,
	SERJIO_GPT_UPDATE_CANCELLED,
	MAX_SERJIO_GPT_UPDATE,
};

static const char *get_serjio_gpt_upd_state_str(enum serjio_gpt_upd_state gpt_upd_state)
{
	switch (gpt_upd_state) {
	case SERJIO_GPT_UPDATE_IDLE: return "IDLE";
	case SERJIO_CHECK_GPT_PRE_WRITE: return "CHECK_GPT_PRE_WRITE";
	case SERJIO_GPT_PRE_WRITE_DONE: return "GPT_PRE_WRITE_DONE";
	case SERJIO_CHECK_GPT_POST_WRITE: return "CHECK_GPT_POST_WRITE";
	case SERJIO_GPT_POST_WRITE_DONE: return "GPT_POST_WRITE_DONE";
	case SERJIO_GPT_UPDATE_CANCELLED: return "GPT_UPDATE_CANCELLED";
	default: return "Unknown Error!";
	}
}

struct seg_tree_entry {
	struct rb_node node;
	int gpt_idx;
	u64 slba;
	u64 elba;
	u64 __subtree_last; /* (elba of the last node of this subtree - used internally by interval_tree_generic) */
	union nvmeib_uuid seg_uuid;
	union nvmeib_uuid seg_type;
	char seg_uuid_str[NVMEIB_GID_STR_MAX];

	/* Used for entry into hash table */
	struct hlist_node link;

	/* Used for Clean Segment */
	struct list_head cln_link; /* Entry into state lists */
	spinlock_t cln_lock;
	enum {
		CLN_SEG_IDLE = 0,
		CLN_SEG_WAIT_SCAN,
		CLN_SEG_SCAN,
		CLN_SEG_WAIT_RNG_RETURN,
		CLN_SEG_DONE,
	} cln_state;
	int wait_rng_cnt;
	DECLARE_BITMAP(wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES);
	bool seg_delete;
	bool deprecated;

	/* Used for JGC */
	u64 jgc_launched_jif;

	/* Used for scanning new GPT */
	bool exists_in_new_gpt;
};

#define SEG_START(seg_tree_entry) ((seg_tree_entry)->slba)
#define SEG_END(seg_tree_entry) ((seg_tree_entry)->elba)

INTERVAL_TREE_DEFINE(struct seg_tree_entry, node, u64, __subtree_last, SEG_START, SEG_END, static, seg_tree);

int nvmeibs_disk_info_get_block_shift(struct nvmeibs_disk_info const * const di){
	BUG_ON(!di);
	return di->block_shift;
}

bool nvmeibs_disk_info_is_ready_for_serjio(struct nvmeibs_disk_info const * const di){
	return di && di->priv;
}

char const * nvmeibs_disk_info_get_disk_id(struct nvmeibs_disk_info const * const di){
	return di ? di->disk_id : "NULL";
}

struct nvmeibs_serjio_disk_private_data*
nvmeibs_disk_info_get_serjio_private_data(struct nvmeibs_disk_info const * const di){
	if (di && di->priv){
		return ((struct nvmeibs_disk_private_data*) di->priv)->serjio_pd;
	}
	return NULL;
}

void nvmeibs_disk_info_set_serjio_private_data(struct nvmeibs_disk_info *di,
	struct nvmeibs_serjio_disk_private_data* serjio_pd)
{
	BUG_ON(!nvmeibs_disk_info_is_ready_for_serjio(di));
	((struct nvmeibs_disk_private_data*) di->priv)->serjio_pd = serjio_pd;
}


bool nvmeibs_disk_info_has_gpt(struct nvmeibs_disk_info const * const di)
{
	if (di && di->priv){
		return !(((struct nvmeibs_disk_private_data*) di->priv)->serjio_err.no_gpt);
	}
	return false;
}

void nvmeibs_disk_info_set_gpt_status(struct nvmeibs_disk_info *di, bool gpt_presents)
{
	BUG_ON(!nvmeibs_disk_info_is_ready_for_serjio(di));
	((struct nvmeibs_disk_private_data*) di->priv)->serjio_err.no_gpt = !gpt_presents;
}

int nvmeibs_disk_info_get_md_size(struct nvmeibs_disk_info const * const di)
{
	if (!di){
		return 0;
	}
	return di->metadata;
}

u64 nvmeibs_disk_info_get_num_blks(struct nvmeibs_disk_info const * const di, bool hw_blocks)
{
	if (hw_blocks)
		return di->hw_blocks;
	else
		return di->blocks;
}

bool nvmeibs_disk_info_has_mtdt_extd(struct nvmeibs_disk_info const * const di) {
	return di->mtdt_extd;
}



struct nvmeibs_serjio_disk_private_data {
	struct nvmeibs_disk_info *di;
	/* SERJIO Boot ID */
	char boot_id[NVMEIB_GID_STR_MAX];

	spinlock_t lock;

	struct {
		size_t len;
		int n_pages;
		struct page **pages;
		struct nvmeib_ref refcount;
	} jmdc_mem;

	/* list of jmdc_mapping for each nic present on the target */
	struct list_head jmdc_mems;
	spinlock_t jmdc_mem_lock;
	struct jranges_allocation_table jranges_alloc_tbl;
	struct nvmeibs_serjio_disk_ranges disk_ranges;

	/* Pool for NVME Ops */
	struct nvme_op_rsrc_pool nvme_op_rsrc_pool;

	/* IO WQ */
	struct workq_struct *io_wq;
	int io_wq_pid;
	atomic_t io_wq_cnt;
	struct completion io_wq_cmp;
	struct list_head io_wq_pend_list;
	unsigned io_wq_pend_cnt;
	spinlock_t io_wq_pend_lock;

	/* For Proc FS */
	struct proc_dir_entry *disk_proc_dir;
	struct nvmeib_public_procfs_ent *boot_id_proc_file;
	struct nvmeib_public_procfs_ent *clients_csv_file;
	struct nvmeib_public_procfs_seq_ent *ranges_csv_file;
	struct nvmeib_public_procfs_ent *abnd_entry_proc_file;
	struct nvmeib_public_procfs_ent *free_entry_proc_file;
	struct nvmeib_public_procfs_ent *unknown_entry_proc_file;
	struct proc_dir_entry *jmdc_proc_dir;
	struct nvmeib_public_procfs_seq_ent **jmdc_proc_files;
	struct nvmeib_public_procfs_ent *jmdc_mapping_proc_file;
	struct nvmeib_public_procfs_ent *jmdc_set_proc_file;
	struct nvmeib_public_procfs_ent *jmdc_set_ext_proc_file;
	struct nvmeib_public_procfs_ent *parts_proc_file;
	struct nvmeib_public_procfs_ent *free_blkset_entries_proc_file;
	struct proc_dir_entry *jents_proc_dir;
	struct nvmeib_public_procfs_seq_ent **rng_ents_proc_files;
	struct nvmeib_public_procfs_ent *serjio_stats_proc_file;
	struct nvmeib_public_procfs_ent *serjio_stats_readable_proc_file;

	spinlock_t state_lock;
	enum nvmeibs_serjio_state state;

	/* GPT Stuff */
	struct gpt_header gpt_hdr;
	struct gpt_entry *gpt_entry;
	unsigned n_gpt_ents;
	size_t gpt_ents_sz;
	struct rw_semaphore gpt_rwsem;

	TIMER_LIST_INSTANCE(jgc_timer);
	TIMER_LIST_INSTANCE(wq_pend_timer);

	/* rbtree used for finding journalled segments by lba (and deleted journalled segments) */
#if KS_RB_ROOT_CACHED
	struct rb_root_cached jrnl_seg_rb_root;
	struct rb_root_cached del_seg_rb_root;
#else
	struct rb_root jrnl_seg_rb_root;
	struct rb_root del_seg_rb_root;
#endif
	/* hash table used for finding journalled segments by uuid */
	DECLARE_HASHTABLE(jrnl_seg_tbl, 8);
	DECLARE_HASHTABLE(del_seg_tbl, 8);
	/* radix-tree used for finding journalled segments by GPT index */
	struct radix_tree_root jrnl_seg_radix_root;
	/* Used for finding journalled segments in clean states */
	spinlock_t cln_seg_list_lock;
	struct list_head cln_seg_wait_scan_list;
	struct list_head cln_seg_scan_list;
	struct list_head cln_seg_wait_ret_list;

	/* Pending GPT Update */
	enum serjio_gpt_upd_state gpt_upd_state;
	bool pend_gpt_primary;
	struct gpt_header *pend_gpt_hdr;
	struct gpt_entry *pend_gpt_ents;
	unsigned n_pend_gpt_ents;
	size_t pend_gpt_ents_sz;

	/* Local copy of modparam nvmeibs_jrange_num_blocks */
	u32 rng_nblk;
	u32 rng_nlba;

	struct nvmeibs_serjio_stats stats;
	enum nvmeibs_serjio_work_type current_work_type;
};

static inline u32 hash_uuid_str(const char *uuid_str)
{
	return *(u32*)uuid_str;
}

#define DECLARE_IO_WQ_FN(fn) int fn(struct nvmeibs_serjio_disk_private_data *serjio_pd, void *param, bool __attribute__((unused)) *resched)
typedef DECLARE_IO_WQ_FN((*io_wq_fn_type));


static int __run_on_io_wq_fn(bool is_inline, io_wq_fn_type fn, struct nvmeibs_serjio_disk_private_data *serjio_pd,
	void *param, bool *resched, struct nvmeibs_serjio_work_stats *work_stats)
{
	int	rv;
	enum nvmeibs_serjio_work_type current_work_type_save = serjio_pd->current_work_type;
	serjio_pd->current_work_type = work_stats->work_type;
	nvmeibs_serjio_update_work_stats_exec(work_stats, &serjio_pd->stats);
	_NDs(t0_run_io_wq_fn, serjio_pd, "--> fn=@FN (inline=@BOOL_YN), @SERJIO_WORK_TYPE", fn, is_inline, serjio_pd->current_work_type);
	rv = (*fn)(serjio_pd, param, resched);
	_NDs(t1_run_io_wq_fn, serjio_pd, "<-- fn=@FN, rv=@RV (inline=@BOOL_YN), @SERJIO_WORK_TYPE", fn, rv, is_inline, serjio_pd->current_work_type);
	if (!*resched) {
		nvmeibs_serjio_update_work_stats_completed(work_stats, &serjio_pd->stats, rv);
	} else {
		nvmeibs_serjio_update_work_stats_resched(work_stats);
	}
	serjio_pd->current_work_type = current_work_type_save;
	return rv;
}

struct run_iowq_workqe {
	struct workqe_struct work;
	io_wq_fn_type fn;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	void *param;
	int *rv;
	struct completion *comp;
	struct list_head pend_link;
	unsigned long pend_jif;
	struct nvmeibs_serjio_work_stats work_stats;
};

static int run_on_io_wq(struct nvmeibs_serjio_disk_private_data *serjio_pd, io_wq_fn_type fn,
						void *param, bool wait_complete, bool can_sleep, enum nvmeibs_serjio_work_type work_type);

static struct nvme_op_rsrc *get_free_nvme_op_rsrc_sync(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, int max_attempts, bool can_schedule);
static void set_nvme_op_rsrc_md(struct nvme_op_rsrc *rsrc, const void *md, size_t md_sz);
static void init_nvme_op_rsrc_io(struct nvme_op_rsrc *rsrc,
		struct nvmeibs_serjio_disk_private_data *serjio_pd, int nvme_cmd,
		bool hw_blocks, sector_t disk_block, size_t data_len, nvme_callback_t *cb, void *cb_param, const void *md, size_t md_sz);
static void read_serjio_db_cb(void *arg, int status, u32 result);
static int send_jam_abnd_free(struct nvmeibs_serjio_disk_private_data *serjio_pd,
			      struct jrange_entry *jrange_entry, unsigned long *abnd_free_bmp);
static int write_jrange_to_serjio_db(struct jrange_entry *jrange_entry,
									 struct completion *comp, atomic_t *comp_ctr);
static int set_jmdc_entry_data(struct jrange_entry *jrange_entry, int entry_idx, const void *data);
static void sync_jmdc_range_to_all_nics(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, int range_idx);
static int alloc_jmdc(struct nvmeibs_serjio_disk_private_data *serjio_pd);
static void free_jmdc_memory(struct nvmeibs_serjio_disk_private_data *serjio_pd);

static int chk_wait_ret_cln_disk_rng(struct jrange_entry *jrange_entry);

static int read_jrnl_entry(struct jrange_entry *jrng, unsigned entry_idx, atomic_t *read_ctr,
						   struct completion *read_comp, nvme_callback_t read_cb, void *cb_param);

static void chk_launch_new_jgc(struct nvmeibs_serjio_disk_private_data *serjio_pd, bool on_boot);

static union jblock_md *get_jmdc_block(struct jrange_entry *jrng, unsigned block_idx);
static union jblock_md *get_jmdc_entry(struct jrange_entry *jrng, unsigned entry_idx);
static struct jrange_entry* get_jrange_entry_for_uuid(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, uuid_be client_uuid, binje_t binje, bool already_locked);

static int rd_jrange(struct nvmeibs_serjio_disk_private_data *serjio_pd,
				   nvme_callback_t read_cb, void *cb_param, struct jrange_entry *jrng,
				   unsigned long read_ent_state_mask, unsigned long *read_ent_bmp,
				   atomic_t *ctr, struct completion *comp);

static void clr_seg_tree_hash(struct nvmeibs_serjio_disk_private_data *serjio_pd);

static struct seg_tree_entry *get_j2d_cln_rng(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u64 j2d, u32 cln_state_mask,
	int rng_idx, bool set_wait_return);

static bool is_j2d_in_valid_segment(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u64 j2d_start, u64 j2d_end, int *seg_ent, union nvmeib_uuid *seg_id);

static int gpt_hdr_num_parts(struct nvmeibs_serjio_disk_private_data *serjio_pd,
							 bool primary, struct gpt_header *gpt_hdr, struct gpt_entry *gpt_ents, size_t gpt_ents_sz);

static void set_rng_binje(struct jrange_entry *jrng, int new_binje_shift,
			  enum nvmeibs_serjio_jentry_state old_valid_ent_state,
			  enum nvmeibs_serjio_jentry_state new_valid_ent_state,
			  unsigned long *ents_to_zero_bmp);

/* State Machine WQ Functions */
static DECLARE_IO_WQ_FN(io_gpt_upd_fn);
static DECLARE_IO_WQ_FN(io_rd_gpt_fn);
static DECLARE_IO_WQ_FN(io_rd_db_fn);
static DECLARE_IO_WQ_FN(io_rd_jrnl_fn);
static DECLARE_IO_WQ_FN(io_ready_fn);
static DECLARE_IO_WQ_FN(io_cln_jrnl_disk_rng_fn);
static DECLARE_IO_WQ_FN(io_init_jrnl_fn);
static DECLARE_IO_WQ_FN(io_chk_jgc_fn);
static DECLARE_IO_WQ_FN(io_alloc_rng_fn);
static DECLARE_IO_WQ_FN(io_ret_rng_fn);
static DECLARE_IO_WQ_FN(io_cln_jrnl_disk_rng_start_fn);

struct io_alloc_rng_param {
	u32 client_id;
	uuid_be client_uuid;
	const char *client_host;
	int binje_req_shift;
	const struct nvmeib_jrange_cache *jrc;
	struct volume_server_get_jrange_rsp *net_rsp;
	struct nvmeib_jrange_rsp *rsp;
};

static inline int alloc_journal_range(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u32 client_id, uuid_be client_uuid,
	const char *client_host, int binje_req_shift, const struct nvmeib_jrange_cache *jrc,
	struct nvmeib_jrange_rsp *local_rsp)
{
	struct io_alloc_rng_param param = {
		.client_id = client_id,
		.client_uuid = client_uuid,
		.client_host = client_host,
		.binje_req_shift = binje_req_shift,
		.jrc = jrc,
		.rsp = local_rsp,
	};
	return run_on_io_wq(
		serjio_pd, io_alloc_rng_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_ALLOC_RNG);
}

static inline int return_journal_range(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u32 cl_jrnl_rng)
{
	long rng_param = cl_jrnl_rng;
	int rv = run_on_io_wq(serjio_pd, io_ret_rng_fn, (void *)rng_param, false, true, NVMEIBS_SERJIO_WORK_TYPE_RET_RNG);
	if (rv == -EINPROGRESS)
		rv = 0;
	return rv;
}

static int chk_rng_post_free(struct jrange_entry *jrange_entry, struct completion *comp, atomic_t *ctr, enum nvmeibs_serjio_jentry_state_chng_reason chng_ent_reason);

static struct proc_dir_entry *serjio_proc_dir = NULL;
static atomic_t num_serjio_disks;

static inline u64 hash_uuid(uuid_be uuid)
{
	u64 *u64ptr = (u64*)&uuid;
	return u64ptr[0] ^ u64ptr[1];
}

static enum nvmeibs_serjio_state serjio_state_cmp_exch(
	struct nvmeibs_serjio_disk_private_data *serjio_pd,
	enum nvmeibs_serjio_state cmp, enum nvmeibs_serjio_state exch)
{
	enum nvmeibs_serjio_state ret;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->state_lock, flags);
	ret = serjio_pd->state;
	if (ret == cmp) {
		serjio_pd->state = exch;
		nvmeibs_serjio_on_state_change(&serjio_pd->stats, ret, exch);
		if (SERJIO_ERROR(exch))
		_NTs(trace_nvmeibs_serjio_c_788, serjio_pd, "@SERJIO_STATE => @SERJIO_STATE", ret, exch);
	}
	spin_unlock_irqrestore(&serjio_pd->state_lock, flags);
	return ret;
}

static enum nvmeibs_serjio_state serjio_state_exch(
	struct nvmeibs_serjio_disk_private_data *serjio_pd,
	enum nvmeibs_serjio_state exch)
{
	enum nvmeibs_serjio_state ret;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->state_lock, flags);
	ret = serjio_pd->state;
	serjio_pd->state = exch;
	nvmeibs_serjio_on_state_change(&serjio_pd->stats, ret, exch);
	if (SERJIO_ERROR(exch))
		_NTs(trace_nvmeibs_serjio_c_809, serjio_pd, "@SERJIO_STATE => @SERJIO_STATE", ret, exch);
	spin_unlock_irqrestore(&serjio_pd->state_lock, flags);
	return ret;
}

static bool serjio_state_cmp(
	struct nvmeibs_serjio_disk_private_data *serjio_pd,
	enum nvmeibs_serjio_state cmp)
{
	bool ret;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->state_lock, flags);
	ret = serjio_pd->state == cmp;
	spin_unlock_irqrestore(&serjio_pd->state_lock, flags);
	return ret;
}

static enum nvmeibs_serjio_state serjio_state_get(
	struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	enum nvmeibs_serjio_state ret;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->state_lock, flags);
	ret = serjio_pd->state;
	spin_unlock_irqrestore(&serjio_pd->state_lock, flags);
	return ret;
}

static inline int jentry_state_chng(struct jrange_entry *rng, unsigned entry,
				enum nvmeibs_serjio_jentry_state next_state, bool already_locked,
				enum nvmeibs_serjio_jentry_state_chng_reason chng_reason,
				const char *fn, int line)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = rng->serjio_pd;
	enum nvmeibs_serjio_jentry_state prev_state;
	int rv = 0;
	unsigned long flags = 0;
	u8 prev_gen_id;

	if (!already_locked)
		spin_lock_irqsave(&rng->lock, flags);
	if (entry < 0 || entry >= rng->n_ents) {
		_NTs(trace_serjio_jentry_state_chng, serjio_pd, "Invalid entry @JRNL_RNG_ENT_IDX", entry);
		rv = -EINVAL;
		goto out;
	}
	prev_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, entry);
	if (prev_state == next_state) {
		/* Nothing to do */
		goto out;
	}
	SET_JENTRY_STATE_IN_BMP(rng->jentry_state_bmp, entry, next_state);
	rng->jentry_state_cnt[prev_state]--; //JJJ
	rng->jentry_state_cnt[next_state]++; //JJJ
	if (rng->jentry_state_cnt[prev_state] > rng->n_ents) {
		_NE(jentry_state_chng_e1, "rng=@PTR, invalid counters transition entry=@INT, @INT-->@INT",
		   rng, entry, prev_state, next_state);
		BUG();
	}
	if (rng->jentry_state_cnt[next_state] > rng->n_ents) {
		_NE(jentry_state_chng_e2, "rng=@PTR, invalid counters transition entry=@INT, @INT-->@INT",
		   rng, entry, prev_state, next_state);
		BUG();
	}
	prev_gen_id = rng->jentry_md[entry].ent_gen_id;
	if (next_state == JENTRY_FREE || next_state == JENTRY_SYNCED || next_state == JENTRY_IO_ERR) {
		/* Increase the Entry GenID on any transition that would invalidate a recovery result */
		if (rng->jentry_md[entry].ent_gen_id < nvmeib_jrnl_ent_gen_id_max) {
			rng->jentry_md[entry].ent_gen_id++;
		} else {
			/* Entry Gen ID hit max value => Set to min, and mark the range gen id to be increased */
			rng->jentry_md[entry].ent_gen_id = nvmeib_jrnl_ent_gen_id_min;
			rng->ent_gen_id_wrapped = true;
		}
	}
	rv = prev_state;

	_NTs_short(trace_1_serjio_jentry_state_chng, serjio_pd,
	     "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX @JENTRY_STATE => @JENTRY_STATE (@JRNL_ENT_GEN_ID) - Reason: @JENTRY_STATE_CHNG_REASON",
	     rng->range_idx, entry, prev_state, next_state, rng->jentry_md[entry].ent_gen_id, chng_reason);

	_NDs(debug_1_serjio_jentry_state_chng, serjio_pd,
	     "Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX @JENTRY_STATE (@JRNL_ENT_GEN_ID) => @JENTRY_STATE (@JRNL_ENT_GEN_ID) - Reason: @JENTRY_STATE_CHNG_REASON From: [@FNAME](@LINENO)",
		 rng->range_idx, 1 << rng->binje_shift, entry, prev_state,
		 prev_gen_id, next_state, rng->jentry_md[entry].ent_gen_id, chng_reason, fn, line);

out:
	if (!already_locked)
		spin_unlock_irqrestore(&rng->lock, flags);
	return rv;
}

#define JENTRY_STATE_CHNG(rng, ent, next, locked, reason) \
	jentry_state_chng(rng, ent, next, locked, reason, __FUNCTION__, __LINE__)

static inline unsigned get_jentry_in_mask_cnt(struct jrange_entry *rng, unsigned mask)
{
	int i;
	unsigned cnt = 0;
	for (i = 0; i < MAX_JENTRY_STATE; i++) {
		if (JENTRY_STATE_IN_MASK(i, mask))
			cnt += rng->jentry_state_cnt[i];
	}
	return cnt;
}

#define IS_SERJIO_DYING(serjio_pd) \
	serjio_state_cmp(serjio_pd, SERJIO_DYING)

#define IS_SERJIO_READY(serjio_pd) \
	serjio_state_cmp(serjio_pd, SERJIO_READY)

#define SET_SERJIO_DYING(serjio_pd) \
	serjio_state_exch(serjio_pd, SERJIO_DYING)

/* Compare GPT Entry partition_type_guid with NVMesh GUIDs in nvmeib_shared.h */
static bool is_gpt_ent_toma_md(const struct gpt_entry *gpt_entry)
{
	const union nvmeib_uuid nvmeib_uuid = NVMESH_METADATA_PARTITION_TYPE_GUID_CONST;
	return memcmp(&gpt_entry->partition_type_guid, &nvmeib_uuid, sizeof(nvmeib_uuid)) == 0;
}

static bool is_gpt_ent_jrnl(const struct gpt_entry *gpt_entry)
{
	const union nvmeib_uuid nvmeib_uuid = NVMESH_JOURNAL_DATA_PARTITION_TYPE_GUID_CONST;
	return memcmp(&gpt_entry->partition_type_guid, &nvmeib_uuid, sizeof(nvmeib_uuid)) == 0;
}

static bool is_gpt_ent_serjio_db(const struct gpt_entry *gpt_entry)
{
	const union nvmeib_uuid nvmeib_uuid = NVMESH_SERJIO_DB_PARTITION_TYPE_GUID_CONST;
	return memcmp(&gpt_entry->partition_type_guid, &nvmeib_uuid, sizeof(nvmeib_uuid)) == 0;
}

static bool is_gpt_ent_seg_jrnl(const struct gpt_entry *gpt_entry, bool *deprecated)
{
	const union nvmeib_uuid nvmeib_uuid = NVMESH_DATA_PARTITION_TYPE_GUID_JOURNALED_CONST;
	if (memcmp(&gpt_entry->partition_type_guid, &nvmeib_uuid, sizeof(nvmeib_uuid)) == 0) {
		if (deprecated)
			*deprecated = (gpt_entry->attributes & NVMESH_JOURNAL_DATA_PARTITION_ATTRIBUTE_DEPRECATED_MASK);
		return true;
	}
	return false;
}

static bool is_gpt_ent_seg_no_jrnl(const struct gpt_entry *gpt_entry)
{
	const union nvmeib_uuid nvmeib_uuid = NVMESH_DATA_PARTITION_TYPE_GUID_NO_JOURNAL_CONST;
	return memcmp(&gpt_entry->partition_type_guid, &nvmeib_uuid, sizeof(nvmeib_uuid)) == 0;
}

static int map_jmdc_at_device(struct nvmeibs_dev *nic,
	struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int ret = 0;
	unsigned int i;
	struct jmdc_mapping* jmdc_map = 0;
	struct scatterlist *sg;
	unsigned long flags;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_map_jmdc_at_device, "disk has no SERJIO private data");
		ret = -ENODATA;
		goto out;
	}

	if (IS_SERJIO_DYING(serjio_pd)) {
		_NTs(trace_3_serjio_map_jmdc_at_device, serjio_pd,
			"SERJIO for disk @DISK_ID_STR is going down",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		ret = -EBUSY;
		goto out;
	}

	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_for_each_entry(jmdc_map, &serjio_pd->jmdc_mems, link) {
		if (jmdc_map->nic_dev == nic) {
			_NTs(trace_serjio_map_jmdc_at_device, serjio_pd, "disk is already mapped to a nic @NIC   pd @DI", nic, serjio_pd->di);
			spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);
			ret = -EALREADY;
			goto out;
		}
	}
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);

	if (!nvmeib_ref_get(&serjio_pd->jmdc_mem.refcount)) {
		_NEs(error_2_serjio_map_jmdc_at_device, serjio_pd, "failed to get reference for jmdc");
		ret = -EACCES;
		goto out;
	}

	jmdc_map = kzalloc(sizeof(*jmdc_map), GFP_KERNEL);
	if (!jmdc_map) {
		_NEs(error_1_serjio_map_jmdc_at_device, serjio_pd, "failed allocating memory for jmdc");
		ret = -ENOMEM;
		nvmesh_memmgr_metric_on_alloc_update(serjio_jmdc_map, sizeof(*jmdc_map), false /* success */);
		goto put_jmdc_mem;
	}
	nvmesh_memmgr_metric_on_alloc_update(serjio_jmdc_map, sizeof(*jmdc_map), true /* success */);

	_NT(trace_1_serjio_map_jmdc_at_device, "mapping jmdc memory to nic @NIC disk_pd=@DISK_PD   jmdc=@JMDC", nic, serjio_pd->di, jmdc_map);
	jmdc_map->nic_dev = nic;
	jmdc_map->serjio_pd = serjio_pd;
	jmdc_map->jmdc_area_map.pages = serjio_pd->jmdc_mem.pages;
	jmdc_map->jmdc_area_map.n_pages = serjio_pd->jmdc_mem.n_pages;
	jmdc_map->jmdc_area_map.rkey = nvmeibs_serjio_map_on_dev(nic, &jmdc_map->jmdc_area_map);
	if (jmdc_map->jmdc_area_map.rkey == 0) {
		ret = -ENOMEM;
		_NEs(error_3_serjio_map_jmdc_at_device, serjio_pd, "Failed to allocate jmdc memory on device");
		goto free_map;
	}
	/* Determine if sgl is linear wrt to jrange index (otherwise we have to loop over the whole sgl when we do sync) */
	jmdc_map->sg_linear_len = jmdc_map->jmdc_area_map.mem_table.sgl[0].length;
	for_each_sg(jmdc_map->jmdc_area_map.mem_table.sgl,
			sg, jmdc_map->jmdc_area_map.mem_table.nents, i)
	{
		if (sg->length != jmdc_map->sg_linear_len || sg->offset != 0) {
			/* SG entry has different length or non-zero offset, not linear */
			jmdc_map->sg_linear_len = 0;
			break;
		}
		if (i + 1 < jmdc_map->jmdc_area_map.mem_table.nents &&
			!sg_is_last(sg) && sg_is_chain((sg + 1)))
		{
			/* SG table is chained, not linear */
			jmdc_map->sg_linear_len = 0;
			break;
		}
	}
	if (!jmdc_map->sg_linear_len) {
		_NTs(trace_4_serjio_map_jmdc_at_device, serjio_pd,
		     "jmdc mapping on ib device @IB_DEVICE is not linear",
			nic->dev->ib_dev->name);
	}
	_NTs(trace_2_serjio_map_jmdc_at_device, serjio_pd, "rkey=@RKEY", jmdc_map->jmdc_area_map.rkey);
	nvmeib_ref_init(&jmdc_map->refcount);
	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_add_tail(&jmdc_map->link, &serjio_pd->jmdc_mems);
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);

	goto out;

free_map:
	kfree(jmdc_map);
	nvmesh_memmgr_metric_on_free_update(serjio_jmdc_map, sizeof(*jmdc_map));

put_jmdc_mem:
	nvmeib_ref_put(&serjio_pd->jmdc_mem.refcount);

out:
	NFOUT;
	return ret;
}

int nvmeibs_serjio_map_all_jmdc_for_nic(struct nvmeibs_dev *nic)
{
	int rv = 0, n_disks;
	struct list_head *disks;
	struct nvmeibs_disk_info *di;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;

	NFIN;
	disks = nvmeibs_serjio_get_disks(&n_disks);
	_ND(trace_serjio_nvmeibs_serjio_map_all_jmdc_for_nic, "SERJIO: found @N_DISKS disks", n_disks);
	list_for_each_entry(di, disks, link) {
		_ND(trace_1_serjio_nvmeibs_serjio_map_all_jmdc_for_nic, "SERJIO: mapping JMD Cache for disk @DISK_ID_STR", nvmeibs_disk_info_get_disk_id(di));
		if (!nvmeibs_disk_info_is_ready_for_serjio(di)) {
			_NT(trace_2_serjio_nvmeibs_serjio_map_all_jmdc_for_nic, "SERJIO: empty disk info of disk @DISK_ID_STR", nvmeibs_disk_info_get_disk_id(di));
			continue;
		}
		serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di);
		if (serjio_pd) {
			if ((rv = map_jmdc_at_device(nic, serjio_pd))) {
				if (rv == -EACCES) {
					_NT(trace_5_serjio_nvmeibs_serjio_map_all_jmdc_for_nic,
					    "SERJIO on Disk @DISK_NAME: JMDC is not ready", di->disk_id);
					rv = 0;
				}
				else if (rv == -EALREADY) {
					rv = 0;
				} else
					goto out;
			}
		} else
			_NT(trace_3_serjio_nvmeibs_serjio_map_all_jmdc_for_nic, "SERJIO: Disk has no serjio data");
	}
	_ND(trace_4_serjio_nvmeibs_serjio_map_all_jmdc_for_nic, "SERJIO: finished to map SERJIO memory at device @IB_DEV_NAME",
	   nic->dev->ib_dev->name);

	out:
	nvmeibs_serjio_put_disks();
	NFOUT;
	return rv;
}

int nvmeibs_serjio_map_disk_jmd_cache(struct nvmeibs_disk_info *di, struct nvmeibs_dev *nic)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	int rv = 0;
	if (!nvmeibs_disk_info_get_md_size(di)) {
		_NT(trace_serjio_nvmeibs_serjio_map_disk_jmd_cache, "SERJIO: Disk @DISK_ID_STR has no meta-data", nvmeibs_disk_info_get_disk_id(di));
		rv = -ENODATA;
		goto out;
	}
	if (!(serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di))) {
		_NT(trace_1_serjio_nvmeibs_serjio_map_disk_jmd_cache, "SERJIO: Disk @DISK_ID_STR has no SERJIO private data", nvmeibs_disk_info_get_disk_id(di));
		rv = -ENODATA;
		goto out;
	}
	if ((rv = map_jmdc_at_device(nic, serjio_pd))) {
		if (rv == -EACCES)
			_NT(trace_3_serjio_nvmeibs_serjio_map_disk_jmd_cache,
			    "SERJIO on Disk @DISK_NAME: JMDC is not ready", nvmeibs_disk_info_get_disk_id(di));
		else
			_NT(trace_2_serjio_nvmeibs_serjio_map_disk_jmd_cache, "SERJIO: Failed (@RV) to map disk @DISK_ID_STR JMD Cache", rv, nvmeibs_disk_info_get_disk_id(di));
	}
out:
	return rv;
}

static int init_jrange_lists(struct nvmeibs_serjio_disk_private_data *serjio_pd, bool allocate)
{
	u32 i, j;
	int rv = 0;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *rng;

	/* Initialise the journal ranges. Before we scan the disk SERJIO DB, all ranges are in an unknown state */
	spin_lock_init(&jranges_alloc_tbl->lock);
	INIT_HLIST_HEAD(&jranges_alloc_tbl->free_ranges);
	INIT_HLIST_HEAD(&jranges_alloc_tbl->invalid_ranges);
	INIT_HLIST_HEAD(&jranges_alloc_tbl->quarantined_ranges);
	hash_init(jranges_alloc_tbl->reserved_ranges);
	if (allocate) {
		jranges_alloc_tbl->ranges = vzalloc(NVMEIB_EC_MAX_JOURNAL_RANGES * sizeof(struct jrange_entry));
		if (!jranges_alloc_tbl->ranges) {
			_NEs(error_serjio_init_jrange_lists, serjio_pd, "Fail to allocate journal ranges");
			rv = -ENOMEM;
			goto out;
		}
	} else {
		memset(jranges_alloc_tbl->ranges, 0, NVMEIB_EC_MAX_JOURNAL_RANGES * sizeof(struct jrange_entry));
		jranges_alloc_tbl->num_free_rngs = 0;
		jranges_alloc_tbl->num_valid_rngs = 0;
		jranges_alloc_tbl->num_reserved_ranges = 0;
		jranges_alloc_tbl->num_db_zero_rngs = 0;
		jranges_alloc_tbl->num_db_err_rngs = 0;
		jranges_alloc_tbl->num_quarantined_rngs = 0;
	}
	for (i = 0, rng = jranges_alloc_tbl->ranges; i < NVMEIB_EC_MAX_JOURNAL_RANGES; i++, rng++) {
		rng->status = JRANGE_INVALID;
		rng->serjio_pd = serjio_pd;
		rng->range_idx = i;
		rng->binje_shift = JRANGE_INVALID_BINJE_SHIFT;
		/* Start range as invalid until we read the DB */
		rng->gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
		rng->rng_rlba = NVMEIB_EC_INVALID_JOURNAL_RLBA;
		rng->rng_nlba = 0;
		rng->rng_rblk = NVMEIB_EC_INVALID_JOURNAL_RBLK;
		rng->rng_nblk = 0;
		rng->n_ents = 0;
		spin_lock_init(&rng->lock);

		for (j = 0; j < NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; j++)
			SET_JENTRY_STATE_IN_BMP(rng->jentry_state_bmp, j, JENTRY_INVALID);

		rng->jentry_state_cnt[JENTRY_INVALID] = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
		hlist_add_head(&rng->link, &jranges_alloc_tbl->invalid_ranges);
	}

out:
	if (allocate) {
		nvmesh_memmgr_metric_on_alloc_update(serjio_entry_table, (NVMEIB_EC_MAX_JOURNAL_RANGES * sizeof(struct jrange_entry)), (rv == 0));
	}
	return rv;
}

static inline size_t calc_jmdc_mem_alloc_sz(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	return (sizeof(*serjio_pd->jmdc_mem.pages) * serjio_pd->jmdc_mem.n_pages) + (serjio_pd->jmdc_mem.n_pages * PAGE_SIZE);
}

static void free_jmdc_memory(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int i;
	NFIN;
	_ND(free_jmdc_memory, "freeing jmdc memory\n");
	nvmeib_ref_release_start(&serjio_pd->jmdc_mem.refcount);
	nvmeib_ref_release_wait(&serjio_pd->jmdc_mem.refcount);

	for (i = 0; i < serjio_pd->jmdc_mem.n_pages; ++i)
		__free_page(serjio_pd->jmdc_mem.pages[i]);
	kfree(serjio_pd->jmdc_mem.pages);
	nvmesh_memmgr_metric_on_free_update(serjio_jmdc_mem, calc_jmdc_mem_alloc_sz(serjio_pd));

	NFOUT;
}

static void free_nvme_op_rsrc(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	unsigned i, j;
	struct nvmeibs_disk_info *di = serjio_pd->di;
	struct device *dev;

	_NDs(trace_serjio_free_nvme_op_rsrc, serjio_pd, "Freeing nvme_op_rsrc");
	dev = di ? nvmeibs_disk_info_get_nvme_dma_device(di) : NULL;
	for (i = 0; i < NUM_OF_NVME_OP_RSRC; i++) {
		struct nvme_op_rsrc *rsrc = &serjio_pd->nvme_op_rsrc_pool.pool[i];
		if (dev) {
			if (rsrc->nvme_phys_virt) {
				for (j = 0; j < rsrc->n_pages; j++) {
					if (rsrc->nvme_phys_virt[j]) {
						dma_unmap_single(
							dev, rsrc->nvme_phys_virt[j], PAGE_SIZE, DMA_BIDIRECTIONAL);
					}
				}
				dma_free_coherent(
					dev, rsrc->nvme_phys_size, rsrc->nvme_phys_virt, rsrc->nvme_phys_dma);
				rsrc->nvme_phys_virt = NULL;
			}
		}
		if (rsrc->virt) {
			free_pages((unsigned long)rsrc->virt, get_order(rsrc->n_pages << PAGE_SHIFT));
			rsrc->virt = NULL;
		}
	}
}

static void reset_nvme_op_rsrc_len_md(struct nvme_op_rsrc *rsrc)
{
	struct nvmeibs_disk_info *di = rsrc->serjio_pd->di;
	rsrc->nvme_req.buf_offset = 0;
	rsrc->nvme_req.data_len = NVMEIBC_SECTOR_SIZE << rsrc->binje_shift;
	if (nvmeibs_disk_info_has_mtdt_extd(di)) {
		rsrc->nvme_req.metadata = NULL;
		rsrc->nvme_req.mtdt_size = 0;
	}
	else {
		rsrc->nvme_req.metadata = rsrc->virt + (rsrc->n_data_pgs << PAGE_SHIFT);
		rsrc->nvme_req.mtdt_size = NVMEIB_D2MD_LEN(rsrc->nvme_req.data_len, nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
	}
}

static void set_nvme_op_rsrc_md(struct nvme_op_rsrc *rsrc, const void *md, size_t md_sz)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = rsrc->serjio_pd;
	struct nvmeibs_disk_info *di = serjio_pd->di;
	size_t calc_sw_md_sz =  (rsrc->nvme_req.data_len >> NVMEIB_EC_JOURNAL_SECTOR_SHIFT) * sizeof(union jblock_md);
	size_t hw_md_sz = NVMEIB_D2MD_LEN(rsrc->nvme_req.data_len,
			nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
	BUG_ON(calc_sw_md_sz > hw_md_sz);
	if (md) {
		SERJIO_BUG_ON(md && calc_sw_md_sz != md_sz, err_serjio_set_nvme_op_rsrc_md_inv_md_sz, serjio_pd,
				"Invalid metadata size @SIZE_T - should be @SIZE_T. nvme_rsrc @PTR",
				md_sz, calc_sw_md_sz, rsrc); \
	} else {
		/* Fill MD with 0s */
		md = page_address(ZERO_PAGE(0));
		md_sz = calc_sw_md_sz;
	}
	if (nvmeibs_disk_info_has_mtdt_extd(di)) {
		size_t copy_sz;
		size_t sw_md_sz = NVMEIB_D2MD_LEN(NVMEIBC_SECTOR_SIZE,
						nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
		void *md_rsrc;
		BUG_ON(rsrc->nvme_req.use_hw_blocks); //Not currently supported
		for (md_rsrc = rsrc->virt + NVMEIBC_SECTOR_SIZE; md_sz > 0;
			 md_rsrc += NVMEIBC_SECTOR_SIZE + sw_md_sz) {
			copy_sz = min(sw_md_sz, md_sz);
			memcpy(md_rsrc, md, copy_sz);
			md += copy_sz;
			md_sz -= copy_sz;
		}
		rsrc->nvme_req.metadata = NULL;
		rsrc->nvme_req.mtdt_size = 0;
	} else {
		rsrc->nvme_req.metadata = rsrc->virt + (rsrc->n_data_pgs << PAGE_SHIFT);
		rsrc->nvme_req.mtdt_size = md_sz;
		memcpy(rsrc->nvme_req.metadata, md, md_sz);
		memset(rsrc->nvme_req.metadata + md_sz, 0, hw_md_sz - md_sz);
	}
}

static void get_nvme_op_rsrc_md(struct nvme_op_rsrc *rsrc, void *md, size_t md_sz)
{
	struct nvmeibs_disk_info *di = rsrc->serjio_pd->di;
	void *md_rsrc;
	size_t sw_md_sz = NVMEIB_D2MD_LEN(NVMEIBC_SECTOR_SIZE, nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
	size_t copy_sz;
	if (nvmeibs_disk_info_has_mtdt_extd(di)) {
		for (md_rsrc = rsrc->virt + NVMEIBC_SECTOR_SIZE; md_sz > 0;
			 md_rsrc += NVMEIBC_SECTOR_SIZE + sw_md_sz) {
			copy_sz = min(sw_md_sz, md_sz);
			memcpy(md, md_rsrc, copy_sz);
			md += copy_sz;
			md_sz -= copy_sz;
		}
	} else {
		md_rsrc = rsrc->virt + (rsrc->n_data_pgs << PAGE_SHIFT);
		memcpy(md, md_rsrc, md_sz);
	}
}

static inline void nvme_op_rsrc_chng_state(struct nvme_op_rsrc *rsrc, enum nvme_op_state exp_state, enum nvme_op_state new_state)
{
	enum nvme_op_state cur_state;
	if ((cur_state = atomic_cmpxchg(&rsrc->state, exp_state, new_state)) != exp_state) {
		struct nvmeibs_serjio_disk_private_data *serjio_pd = rsrc->serjio_pd;
#if !defined(SERJIO_NVME_OP_RSRC_STATE_CALL_STACK) || (defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR)
		_NEs(error_nvmeibs_serjio_c_1226, serjio_pd,
			"nvme_op @PTR in invalid state: @NVME_OP_STATE",
			rsrc, cur_state);
#else
		_NEs(error_nvmeibs_serjio_c_1226, serjio_pd,
			"nvme_op @PTR in invalid state: @NVME_OP_STATE. Call Stack: @FN_FUNC<-@FN_FUNC<-@FN_FUNC",
			rsrc, cur_state,
			(void *)rsrc->state_chng_trace_ents[0],
			(void *)rsrc->state_chng_trace_ents[1],
			(void *)rsrc->state_chng_trace_ents[2]);
#endif
		BUG_ON(1);
	} else {
#if defined(SERJIO_NVME_OP_RSRC_STATE_CALL_STACK) && (!defined(BLKDEV_SIMULATOR) || !BLKDEV_SIMULATOR)
		nvmeib_public_save_stack_trace(&rsrc->state_chng_stack_trace);
#endif
	}
}

static int init_nvme_op_rsrc(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int rv = 0;
	unsigned i, j;
	struct nvme_op_rsrc_pool *nvme_op_rsrc_pool = &serjio_pd->nvme_op_rsrc_pool;
	struct device *dev;
	size_t data_sz, md_sz;
	unsigned n_data_pgs, n_md_pgs;
	struct nvmeibs_disk_info *di;

	INIT_LIST_HEAD(&nvme_op_rsrc_pool->free_list);
	spin_lock_init(&nvme_op_rsrc_pool->lock);
	init_completion(&nvme_op_rsrc_pool->free_comp);
	if (!serjio_pd->di) {
		_NE(error_serjio_init_nvme_op_rsrc, "cannot allocate JMDB memory without disk device");
		rv = -EINVAL;
		goto out;
	}
	di = serjio_pd->di;
	data_sz = NVMEIBC_SECTOR_SIZE * NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY;
	n_data_pgs = DIV_ROUND_UP(data_sz, PAGE_SIZE);
	md_sz = NVMEIB_D2MD_LEN(data_sz, nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
	n_md_pgs = DIV_ROUND_UP(md_sz, PAGE_SIZE);
	dev = nvmeibs_disk_info_get_nvme_dma_device(di);
	for (i = 0; i < NUM_OF_NVME_OP_RSRC; i++) {
		struct nvme_op_rsrc *rsrc = &nvme_op_rsrc_pool->pool[i];
		rsrc->n_data_pgs = n_data_pgs;
		rsrc->n_md_pgs = n_md_pgs;
		rsrc->n_pages = n_data_pgs + n_md_pgs;
		if (!(rsrc->virt = (u64*)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(rsrc->n_pages << PAGE_SHIFT)))) {
			_NEs(error_1_serjio_init_nvme_op_rsrc, serjio_pd, "Couldn't get available pages");
			rv = -ENOMEM;
			goto free_pages;
		}
		rsrc->nvme_phys_size = sizeof(*rsrc->nvme_phys_virt) * rsrc->n_pages;
		rsrc->nvme_phys_virt = dma_alloc_coherent(
			dev, rsrc->nvme_phys_size, &rsrc->nvme_phys_dma, GFP_KERNEL);
		if (!rsrc->nvme_phys_virt) {
			_NE(error_2_serjio_init_nvme_op_rsrc, "Could not allocate DMA memory for PRPL");
			rv = -ENOMEM;
			goto free_pages;
		}
		rsrc->nvme_req.use_sg = false;
		for (j = 0; j < rsrc->n_pages; j++) {
			rsrc->nvme_phys_virt[j] = dma_map_single(
				dev, rsrc->virt + (j << PAGE_SHIFT), PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, rsrc->nvme_phys_virt[j])) {
				_NE(error_3_serjio_init_nvme_op_rsrc, "Error mapping memory to disk device");
				rv = -ENOMEM;
				goto free_pages;
			}
		}
		rsrc->nvme_req.buf_addrs = rsrc->nvme_phys_virt;
		rsrc->nvme_req.prpl_phys = rsrc->nvme_phys_dma;
		rsrc->nvme_req.mtdt_dma_ptr = rsrc->nvme_phys_virt[rsrc->n_data_pgs];
		rsrc->serjio_pd = serjio_pd;
		rsrc->range_idx = -1;
		rsrc->binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
		rsrc->entry = -1;
		rsrc->status = 0;
		rsrc->nvme_req.disk_info = serjio_pd->di;
		reset_nvme_op_rsrc_len_md(rsrc);
		atomic_set(&rsrc->state, NVME_OP_FREE);
#if defined(SERJIO_NVME_OP_RSRC_STATE_CALL_STACK) && (!defined(BLKDEV_SIMULATOR) || !BLKDEV_SIMULATOR)
		rsrc->state_chng_stack_trace.max_entries = NVME_OP_RSRC_MAX_STACK_TRACE;
		rsrc->state_chng_stack_trace.entries = rsrc->state_chng_trace_ents;
		rsrc->state_chng_stack_trace.skip = 1;
#endif
		list_add_tail(&rsrc->link, &nvme_op_rsrc_pool->free_list);
		nvme_op_rsrc_pool->n_free++;
	}
	goto out;

free_pages:
	free_nvme_op_rsrc(serjio_pd);

out:
	return rv;
}

/* Calc Journal Ranges according to Journal and SERJIO DB offset and size */
static int serjio_init_calc_num_jranges(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	struct nvmeibs_disk_info *di = serjio_pd->di;
	unsigned tot_jrnl_blk = 0, num_serjio_db_ents = 0, num_jrnl_rng = 0;
	int rv = 0;

	NFIN;
	if (nvmeibs_disk_info_get_block_shift(di) < NVMEIB_EC_JOURNAL_SECTOR_SHIFT) {
		if (!IS_ALIGNED(serjio_pd->disk_ranges.journal.lba,
						JOURNAL_ENTS_TO_DISK_LBAS(di, NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY, 1))) {
			_NT(serjio_init_calc_num_jranges_jrnl, "journal-slba (@LBA_LLONG) not aligned to journal-sector\n", serjio_pd->disk_ranges.journal.lba);
			rv = -EINVAL;
			goto out;
		}
		if (!IS_ALIGNED(serjio_pd->disk_ranges.db.lba,
						SERJIO_DB_ENTS_TO_DISK_LBAS(di, 1))) {
			_NT(serjio_init_calc_num_jranges_db, "serjio_db_lba (@LBA_LLONG) not aligned to serjio_db entry size\n", serjio_pd->disk_ranges.db.lba);
			rv = -EINVAL;
			goto out;
		}
	}
	num_serjio_db_ents = DISK_LBAS_TO_SERJIO_DB_ENTS(di, serjio_pd->disk_ranges.db.len_nlbas);
	if (num_serjio_db_ents < NVMEIB_EC_MAX_JOURNAL_RANGES) {
		serjio_pd->jranges_alloc_tbl.num_ranges = num_serjio_db_ents;
		_NWs(warn_serjio_serjio_init_calc_num_jranges, serjio_pd,
		     "Maximum number of ranges restricted to @NUM_RANGES due to SERJIO DB size (@LEN_NLBAS lbas)",
			serjio_pd->jranges_alloc_tbl.num_ranges,
			serjio_pd->disk_ranges.db.len_nlbas);
	} else {
		serjio_pd->jranges_alloc_tbl.num_ranges = NVMEIB_EC_MAX_JOURNAL_RANGES;
	}
	tot_jrnl_blk = DISK_LBAS_TO_JOURNAL_BLKS(di, serjio_pd->disk_ranges.journal.len_nlbas);
	num_jrnl_rng = min_t(unsigned, tot_jrnl_blk / nvmeibs_jrange_num_blocks, serjio_pd->jranges_alloc_tbl.num_ranges);
	_NTs(trace_serjio_serjio_init_calc_num_jranges, serjio_pd,
	     "Total Journal Blocks @N_BLOCKS (@LEN_NLBAS lbas). Number of Available Journal Ranges @NUM_RANGES of Jblocks @N_BLOCKS",
		tot_jrnl_blk, serjio_pd->disk_ranges.journal.len_nlbas,
		num_jrnl_rng, nvmeibs_jrange_num_blocks);
out:
	NFOUT;
	return rv;
}

static ssize_t fill_clients_csv(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	int i;
	ssize_t count = 0;
	unsigned long flags;
	struct jrange_entry *jrange_iter;
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	char uuid_str[NVMEIB_GID_STR_MAX];

	count += scnprintf(buffer + count, len - count,
			   "client_id,client_uuid,client_host,range_index\n");
	spin_lock_irqsave(&serjio_pd->jranges_alloc_tbl.lock, flags);
	/* Check if the client has already been reserved a range that it abandoned */
	__hash_for_each_safe__(serjio_pd->jranges_alloc_tbl.reserved_ranges, i,
			       t_node, h_node, jrange_iter, link) {
		count += scnprintf(buffer + count, len - count, "%llu,%s,%.32s,%u\n",
					jrange_iter->client_id, nvmeibs_serjio_uuid_to_str(
						uuid_str, jrange_iter->client_uuid.b),
					jrange_iter->client_host, jrange_iter->range_idx);
	}
	spin_unlock_irqrestore(&serjio_pd->jranges_alloc_tbl.lock, flags);

	return count;
}

#define CORE_SERVER_BOOT_ID_PROC_FRMT_VER 1
static ssize_t fill_boot_id(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	ssize_t count = 0;

	count += scnprintf(buffer, len, "%s\n", serjio_pd->boot_id);
	count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_BOOT_ID_PROC_FRMT_VER, buffer + count, len - count);

	return count;
}

static ssize_t stats_clear(void *arg, char *buffer, size_t len)
{
	int reset;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;

	if (sscanf(buffer, "%d", &reset) != 1 || reset != 0) {
		return -EINVAL;
	}

	nvmeibs_serjio_stats_clear(&serjio_pd->stats);
	return len;
}

static ssize_t fill_serjio_stats(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;

	return nvmeibs_serjio_fill_serjio_stats_json(&serjio_pd->stats, buffer, len);
}

// Human-readable stats printing routine
static ssize_t fill_serjio_stats_readable(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	return nvmeibs_serjio_fill_serjio_stats_readable(&serjio_pd->stats, buffer, len);
}

static const char *jrange_status_csv_header="index,binje,rlba,nlba,rblk,nblk,status,client_id,client_uuid,client_host,unknown_entries,synced_entries,free_entries,abandoned_entries,taken_entries,wait_return_entries,io_error_entries,invalid_entries,last_allocated,last_returned";

static void *fill_ranges_seq_start(struct seq_file *m, loff_t *pos)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = m->private;
	struct jrange_entry *jrange_entry;
	if (IS_SERJIO_DYING(serjio_pd))
		return NULL;
	if (*pos >= serjio_pd->jranges_alloc_tbl.num_ranges)
		return NULL;
	if (*pos == 0)
		seq_printf(m, "%s\n", jrange_status_csv_header);
	jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[*pos];
	return jrange_entry;
}

static void fill_ranges_seq_stop(struct seq_file *m, void *v)
{
	(void)m;
	(void)v;
}

static void *fill_ranges_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct jrange_entry *jrange_entry = v;
	(void)m;
	(*pos)++;
	if (*pos >= jrange_entry->serjio_pd->jranges_alloc_tbl.num_ranges)
		return NULL;
	return (jrange_entry + 1);
}

static int fill_ranges_seq_show(struct seq_file *m, void *v)
{
	struct jrange_entry *jrange_entry = v;
	struct rtc_time last_alloc_tm, last_ret_tm, assigned_tm;
	static const char *month_name[] = {
		"Jan",
		"Feb",
		"Mar",
		"Apr",
		"May",
		"Jun",
		"Jul",
		"Aug",
		"Sep",
		"Oct",
		"Nov",
		"Dec"
	};
	enum nvmeibs_serjio_jentry_state ent_state;
	char uuid_str[NVMEIB_GID_STR_MAX];

	if (jrange_entry->status == JRANGE_FREE ||
		jrange_entry->status == JRANGE_DB_ZERO ||
		jrange_entry->status == JRANGE_UNKNOWN ||
		(jrange_entry->status == JRANGE_INVALID && jrange_entry->range_idx >= NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES))
		return 0;

	rtc_time_to_tm(jrange_entry->last_allocated.tv_sec, &last_alloc_tm);
	rtc_time_to_tm(jrange_entry->last_returned.tv_sec, &last_ret_tm);
	rtc_time_to_tm(jrange_entry->reserved.tv_sec, &assigned_tm);
	seq_printf(m, "%u,%u,%u,%u,%u,%u,%s,%llu,%s,%.32s,",
				jrange_entry->range_idx, 
				jrange_entry->binje_shift == JRANGE_INVALID_BINJE_SHIFT ? 
					NVMEIB_EC_INVALID_JOURNAL_BINJE : (1U << jrange_entry->binje_shift),
				jrange_entry->rng_rlba, jrange_entry->rng_nlba,
				jrange_entry->rng_rblk, jrange_entry->rng_nblk,
				nvmeib_shared_serjio_jrange_status_to_str(jrange_entry->status), jrange_entry->client_id,
				nvmeibs_serjio_uuid_to_str(uuid_str, jrange_entry->client_uuid.b),
				jrange_entry->client_id > 0 ? jrange_entry->client_host : "NONE");

	for (ent_state = JENTRY_UNKNOWN; ent_state < MAX_JENTRY_STATE; ent_state++) {
		#if 0 /* For printing the entire bitmap as a list*/
		DECLARE_BITMAP(ent_state_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
		memset(&ent_state_bmp, 0, sizeof(ent_state_bmp));
		for (i = 0; i < jrange_entry->n_ents; i++) {
			if (GET_JENTRY_STATE_FROM_BMP(jrange_entry->jentry_state_bmp, i) == ent_state)
				set_bit(i, ent_state_bmp);
		}
		seq_printf(m, NVMEIB_EC_JOURNAL_MAXBLKS_PER_RANGE_PRINT ",",
					ent_state_bmp);
		#else
		seq_printf(m, "%d,",
					jrange_entry->jentry_state_cnt[ent_state]);
		#endif
	}
	if (jrange_entry->reserved.tv_sec > 0)
		seq_printf(m, "%s %02d %02d:%02d:%02d,",
					 month_name[assigned_tm.tm_mon],
					 assigned_tm.tm_mday,
					 assigned_tm.tm_hour,
					 assigned_tm.tm_min,
					 assigned_tm.tm_sec);
		else
			seq_printf(m, "0,");
	if (jrange_entry->last_allocated.tv_sec > 0)
		seq_printf(m, "%s %02d %02d:%02d:%02d,",
					month_name[last_alloc_tm.tm_mon],
					last_alloc_tm.tm_mday,
					last_alloc_tm.tm_hour,
					last_alloc_tm.tm_min,
					last_alloc_tm.tm_sec);
	else
		seq_printf(m, "0,");
	if (jrange_entry->last_returned.tv_sec > 0)
		seq_printf(m, "%s %02d %02d:%02d:%02d",
				month_name[last_ret_tm.tm_mon],
				last_ret_tm.tm_mday,
				last_ret_tm.tm_hour,
				last_ret_tm.tm_min,
				last_ret_tm.tm_sec);
	else
		seq_printf(m, "0");
	seq_printf(m, "\n");
	return 0;
}

struct seq_operations fill_ranges_seq_ops = {
	.start = fill_ranges_seq_start,
	.show = fill_ranges_seq_show,
	.next = fill_ranges_seq_next,
	.stop = fill_ranges_seq_stop,
};

static ssize_t fill_jmdc_mapping_csv(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	ssize_t count = 0;
	unsigned long flags;
	struct jmdc_mapping *jmdc_mapping;

	count += scnprintf(buffer + count, len - count,
					   "device,rkey,addr,n_pages\n");

	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_for_each_entry(jmdc_mapping, &serjio_pd->jmdc_mems, link) {
		count += scnprintf(buffer + count, len - count,
					   "%s,%#x,%#llx,%d\n", N2IB(jmdc_mapping->nic_dev)->name,
					   jmdc_mapping->jmdc_area_map.rkey,
					   jmdc_mapping->jmdc_area_map.ioaddr,
					   jmdc_mapping->jmdc_area_map.n_pages);
	}
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);

	return count;
}

static void *fill_jmdc_seq_start(struct seq_file *m, loff_t *pos)
{
	struct jrange_entry *jrng = m->private;
	union jblock_md *jmdc_entry;
	unsigned block_idx = (*pos);
	if (block_idx >= jrng->rng_nblk)
		return NULL;
	if (block_idx == 0)
		seq_printf(m, "jblock,entry,raw,j2d,tx_id,tx_bmp,has_next,version\n");
	seq_printf(m, "%03u,%03u,", block_idx, block_idx >> jrng->binje_shift);
	jmdc_entry = get_jmdc_block(jrng, block_idx);
	return jmdc_entry;
}

static void fill_jmdc_seq_stop(struct seq_file *m, void *v)
{
	(void)m;
	(void)v;
}

static void *fill_jmdc_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct jrange_entry *jrng = m->private;
	union jblock_md *jmdc_entry = v;
	unsigned block_idx;
	(*pos)++;
	block_idx = *pos;
	if (block_idx >= jrng->rng_nblk)
		return NULL;
	seq_printf(m, "%03u,%03u,", block_idx, block_idx >> jrng->binje_shift);
	jmdc_entry = get_jmdc_block(jrng, block_idx);
	return jmdc_entry;
}

static int fill_jmdc_seq_show(struct seq_file *m, void *v)
{
	union jblock_md *jmdc_entry = v;
	const char *format = "%llx,%llu,%#05x,%"__stringify(NVMEIB_EC_JMDC_BITS_TX_BMP)"pb,%u,%u\n";
	u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_entry);
	u32 tx_id;
	unsigned long tx_bmp;
	u32 has_next = 0;
	if (jmdc_entry->version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) {
		tx_id = jmdc_entry->tx_id;
		tx_bmp = jmdc_entry->tx_bmp;
	} else {
		BUG_ON(jmdc_entry->version != NVMEIBC_JOURNAL_MD_VERSION_PACKED);
		tx_id = jmdc_entry->v1.tx_id;
		/* TBD: Decompress ? */
		tx_bmp = jmdc_entry->v1.tx_bmp_zip;
		has_next = jmdc_entry->v1.has_next;
	}
	seq_printf(m, format, jmdc_entry->raw, j2d, tx_id, &tx_bmp, has_next, jmdc_entry->version);
	return 0;
}

struct seq_operations fill_jmdc_seq_ops = {
	.start = fill_jmdc_seq_start,
	.show = fill_jmdc_seq_show,
	.next = fill_jmdc_seq_next,
	.stop = fill_jmdc_seq_stop,
};

static void *fill_jents_csv_seq_start(struct seq_file *m, loff_t *pos)
{
	struct jrange_entry *jrng = m->private;
	if (*pos >= jrng->n_ents)
		return NULL;
	if (*pos == 0)
		seq_printf(m, "entry,state,ent_md,j2d_start,j2d_end,tx_id,tx_bmp,ver,seg_uuid,chain_error,chain_err_block\n");
	down_read(&jrng->serjio_pd->gpt_rwsem);
	return pos;
}

static void fill_jents_csv_seq_stop(struct seq_file *m, void *v)
{
	struct jrange_entry *jrng = m->private;
	if (v)
		up_read(&jrng->serjio_pd->gpt_rwsem);
}

static void *fill_jents_csv_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct jrange_entry *jrng = m->private;
	(void)m;
	(*pos)++;
	if (*pos >= jrng->n_ents) {
		if (v)
			up_read(&jrng->serjio_pd->gpt_rwsem);
		return NULL;
	}
	return pos;
}

static int fill_jents_csv_seq_show(struct seq_file *m, void *v)
{
	struct jrange_entry *jrng = m->private;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	long entry = *(long*)v;
	union jblock_md *jmdc_entry = get_jmdc_entry(jrng, entry);
	u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	struct seg_tree_entry *seg_tree_entry = NULL;
	u32 tx_id = jmdc_entry->tx_id;
	unsigned long tx_bmp = jmdc_entry->tx_bmp;
	const char *format = "%03ld,%s,%d,%llu,%llu,%#05x,%"__stringify(NVMEIB_EC_JMDC_BITS_TX_BMP)"pb,%u,%s,%s,%d\n";
	int chain_err = NVMEIB_JENTRY_CHAIN_BLOCK_NOT_IO_0;

	if (jmdc_entry->version == NVMEIBC_JOURNAL_MD_VERSION_PACKED) {
		tx_id = jmdc_entry->v1.tx_id;
		/* TBD: Decompress ? */
		tx_bmp = jmdc_entry->v1.tx_bmp_zip;
	}

	if (nvmeib_is_jmd_io_entry(*jmdc_entry)) {
		if (!(chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << jrng->binje_shift, &j2d_start, &j2d_end))) {
			if ((seg_tree_entry = seg_tree_iter_first(&serjio_pd->jrnl_seg_rb_root, j2d_start, j2d_start))) {
				SERJIO_BUG_ON(j2d_end > seg_tree_entry->elba, err_fill_jents_csv_seq_show_j2d_overflow, serjio_pd,
						"Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX - J2D [@J2D_START, @J2D_END] Overflows Segment @SEG_UUID_STR with LBA Range [@SLBA_LLONG, @ELBA]",
						jrng->range_idx, (jmdc_entry - get_jmdc_entry(jrng, 0)) >> jrng->binje_shift,
						j2d_start, j2d_end, seg_tree_entry->seg_uuid_str, seg_tree_entry->slba, seg_tree_entry->elba);
			}
		} else {
			_NWs(err_fill_jents_csv_seq_show_inv_chain, serjio_pd,
			"Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX has invalid chain at @PTR. Chain Error @ERR_STR Chain Error Block @IDX",
			jrng->range_idx, entry, jmdc_entry, nvmeib_shared_jentry_md_chain_err_str(chain_err),
			nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
		}
	}
	seq_printf(m, format, entry, nvmeib_shared_serjio_jentry_state_to_str(
		GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, entry)),
		jrng->jentry_md[entry].ent_gen_id, j2d_start, j2d_end, tx_id, &tx_bmp, jmdc_entry->version,
		seg_tree_entry ? seg_tree_entry->seg_uuid_str : "N/A",
		nvmeib_shared_jentry_md_chain_err_str(chain_err),
		nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
	return 0;
}

struct seq_operations fill_jents_csv_seq_ops = {
	.start = fill_jents_csv_seq_start,
	.show = fill_jents_csv_seq_show,
	.next = fill_jents_csv_seq_next,
	.stop = fill_jents_csv_seq_stop,
};

static ssize_t fill_parts_csv(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	ssize_t count = 0;
	u32 i, j;
	struct gpt_entry *gpt_entry;
	static const efi_guid_t gpt_null_guid = NULL_GUID;
	char uuid_str[NVMEIB_GID_STR_MAX];
	bool seg_deprecated;
	unsigned num_parts = serjio_pd->n_gpt_ents;

	NFIN;
	if (IS_SERJIO_DYING(serjio_pd)) {
		count = -EBUSY;
		goto out;
	}
	down_read(&serjio_pd->gpt_rwsem);
	if (!serjio_pd->gpt_entry) {
		count += scnprintf(buffer + count, len - count,
						   "GPT Error\n");
		goto unlock;
	}
	count += scnprintf(buffer + count, len - count,
			   "part_num,start_lba,end_lba,guid,type,name\n");
	for (i = 0, gpt_entry = serjio_pd->gpt_entry; i < num_parts; i++, gpt_entry++) {
		char part_name[ARRAY_SIZE(gpt_entry->partition_name) + 1];
		char type_str[64] = "";
		if (efi_guidcmp(gpt_entry->unique_partition_guid, gpt_null_guid) == 0)
			continue;
		for (j = 0; j < ARRAY_SIZE(gpt_entry->partition_name); j++) {
			part_name[j] = (char)gpt_entry->partition_name[j];
		}
		part_name[j] = 0;
		if (is_gpt_ent_toma_md(gpt_entry))
			strcpy(type_str, "EXCELERO_METADATA");
		else if (is_gpt_ent_jrnl(gpt_entry))
			strcpy(type_str, "EXCELERO_JOURNAL_DATA");
		else if (is_gpt_ent_serjio_db(gpt_entry))
			strcpy(type_str, "EXCELERO_SERJIO_DB");
		else if (is_gpt_ent_seg_jrnl(gpt_entry, &seg_deprecated)) {
			if (seg_deprecated)
				strcpy(type_str, "DATA_JOURNALED (DEPRECATED)");
			else
				strcpy(type_str, "DATA_JOURNALED");
		} else if (is_gpt_ent_seg_no_jrnl(gpt_entry))
			strcpy(type_str, "DATA_NO_JOURNAL");
		else
			sprintf(type_str, "%s", nvmeibs_serjio_uuid_le_to_str(uuid_str, gpt_entry->partition_type_guid.b));
		count += scnprintf(
			buffer + count, len - count, "%d,%llu,%llu,%s,%s,%s\n",
			i, le64_to_cpu(gpt_entry->starting_lba), le64_to_cpu(gpt_entry->ending_lba),
			nvmeibs_serjio_uuid_le_to_str(uuid_str, gpt_entry->unique_partition_guid.b), type_str, part_name);
	}
unlock:
	up_read(&serjio_pd->gpt_rwsem);
out:
	NFOUT;
	return count;
}

/* Set Journal Range Entry State from an External Source */
static int set_jrange_entry_state_external(struct nvmeibs_serjio_disk_private_data *serjio_pd,
										   unsigned int range, unsigned int entry,
										   enum nvmeibs_serjio_jentry_state state)
{
	unsigned long flags;
	int rv = 0;
	struct nvmeib_free_ents_data free_ent = {0};
	struct jrange_entry *rng;

	if (range >= NVMEIB_EC_MAX_JOURNAL_RANGES) {
		_NTs(trace_serjio_set_jrange_entry_state_external, serjio_pd, "Invalid range @JRNL_RNG_IDX", range);
		rv = -EINVAL;
		goto out;
	}

	rng = &serjio_pd->jranges_alloc_tbl.ranges[range];

	if (rng->status != JRANGE_RESERVED && rng->status != JRANGE_ALLOCATED) {
		_NTs(trace_2_serjio_set_jrange_entry_state_external, serjio_pd, "Range in invalid state @JRANGE_STATUS", rng->status);
		rv = -EINVAL;
		goto out;
	}

	if (entry >= rng->n_ents) {
		_NTs(trace_1_serjio_set_jrange_entry_state_external, serjio_pd,
			 "Invalid entry @JRNL_RNG_ENT_IDX for range N @BINJE", entry, 1 << rng->binje_shift);
		rv = -EINVAL;
		goto out;
	}

	/* Entries must stay owned by SERJIO */
	if ((unsigned)state & JENTRY_OWN_JAM_MASK || state == JENTRY_INVALID) {
		_NTs(trace_3_serjio_set_jrange_entry_state_external, serjio_pd, "Invalid entry state @JENTRY_STATE",
			state);
		rv = -EINVAL;
		goto out;
	}
	if (state == JENTRY_FREE) {
		spin_lock_irqsave(&rng->lock, flags);
		free_ent.rng_idx = rng->range_idx;
		free_ent.rng_gen_id = rng->gen_id;
		free_ent.ent_idx = entry;
		free_ent.ent_md = rng->jentry_md[entry];
		free_ent.rng_binje = 1 << rng->binje_shift;
		spin_unlock_irqrestore(&rng->lock, flags);

		rv = nvmeibs_serjio_free_jrnl_ents(
			serjio_pd->di, serjio_pd->boot_id, NULL, NVMEIB_RECOV_SRC_PROC_FILE, 0, 1, &free_ent);
	}
	else {
		spin_lock_irqsave(&rng->lock, flags);
		rv = JENTRY_STATE_CHNG(rng, entry, state, true, JENTRY_STATE_CHNG_REASON_EXTERNAL);
		if (rng->ent_gen_id_wrapped) {
			rng->gen_id++;
			rng->ent_gen_id_wrapped = false;
			spin_unlock_irqrestore(&rng->lock, flags);
			if ((rv = write_jrange_to_serjio_db(rng, NULL, NULL))) {
				_NEs(set_jrange_entry_state_external_e1, serjio_pd, "write_jrange_to_serjio_db failed (@INT) for range @UINT",
					rv, rng->range_idx);
			}
		} else
			spin_unlock_irqrestore(&rng->lock, flags);
	}

out:
	return rv;
}

static ssize_t set_jentry_proc(void *arg, char *buf, size_t len, enum nvmeibs_serjio_jentry_state state)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	unsigned int range, entry;
	ssize_t rv = len;

	if (len < 3) {
		rv = -EINVAL;
		goto out;
	}

	if (sscanf(buf, "%u:%u", &range, &entry) != 2) {
		rv = -EINVAL;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		rv = -EAGAIN;
		goto out;
	}

	if ((rv = set_jrange_entry_state_external(serjio_pd, range, entry, state)) == -EALREADY) {
		_NTs(trace_serjio_set_jentry_proc, serjio_pd, "Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX already set to @JENTRY_STATE",
		    range, entry, state);
		goto out;
	}
	_NTs(trace_1_serjio_set_jentry_proc, serjio_pd, "Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX set to @JENTRY_STATE",
		range, entry, state);

out:
	return rv;
}

static ssize_t set_jrange_abnd_entry(void *arg, char *buf, size_t len)
{
	return set_jentry_proc(arg, buf, len, JENTRY_ABND);
}

static ssize_t set_jrange_unknown_entry(void *arg, char *buf, size_t len)
{
	return set_jentry_proc(arg, buf, len, JENTRY_UNKNOWN);
}

static ssize_t set_jrange_free_entry(void *arg, char *buf, size_t len)
{
	return set_jentry_proc(arg, buf, len, JENTRY_FREE);
}

#ifdef __LITTLE_ENDIAN
	static const u8 __attribute__((unused)) uuid_si_le[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
	static const u8 __attribute__((unused)) uuid_si_be[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
#else
	static const u8 __attribute__((unused)) uuid_si_le[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
	static const u8 __attribute__((unused)) uuid_si_be[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
#endif

static int parse_client_uuid(const char *client_uuid_str, uuid_be *client_uuid)
{
	int rv = 0, i;
	int dash_bytes[4] = {8, 13, 18, 23};
	int start_bytes[16] = {0,2,4,6,9,11,14,16,19,21,24,26,28,30,32,34};

	//   32 hex characters, dash separated: 68d43bb1-2a39-11e5-b50c-afee2ad8a19a
	for (i = 0; i < (int)ARRAY_SIZE(dash_bytes); i++) {
		if (client_uuid_str[dash_bytes[i]] != '-') {
			rv = -EINVAL;
			goto out;
		}
	}
	for (i = 0; i < (int)ARRAY_SIZE(start_bytes); i++) {
		client_uuid->b[uuid_si_be[i]] = hex_to_bin(client_uuid_str[start_bytes[i]]) << 4 |
			hex_to_bin(client_uuid_str[start_bytes[i] + 1]);
	}
out:
	return rv;
}

static ssize_t set_free_blkset_entries(void *arg, char *buf, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	ssize_t rv = len;
	char client_uuid_str[URN_UUID_STR_LENGTH + 1];
	uuid_be client_uuid = NULL_UUID_BE;
	u64 blkset_slba = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	struct nvmeib_free_ents_data *free_ents = NULL;
	unsigned long flags;
	struct jrange_entry *rng;
	unsigned i, num_ents = 0;

	if (len < 3) {
		rv = -EINVAL;
		goto out;
	}

	if (sscanf(buf, "%36s:%llu", client_uuid_str, &blkset_slba) != 2) {
		rv = -EINVAL;
		goto out;
	}
	if ((rv = parse_client_uuid(client_uuid_str, &client_uuid))) {
		_NEs(error_serjio_set_free_blkset_entries, serjio_pd, "uuid_be_to_bin (@RV) failed for @CLIENT_UUID_STR", (int)rv, client_uuid_str);
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		rv = -EAGAIN;
		goto out;
	}

	if (!(rng = get_jrange_entry_for_uuid(serjio_pd, client_uuid, NVMEIB_EC_INVALID_JOURNAL_BINJE, false))) {
		_NTs(trace_serjio_set_free_blkset_entries, serjio_pd, "Client @CLIENT_UUID_STR has no range allocated", client_uuid_str);
		rv = -ENOENT;
		goto out;
	}
	if (!(free_ents = kcalloc(rng->n_ents, sizeof(*free_ents), GFP_KERNEL))) {
		_NEs(error_1_serjio_set_free_blkset_entries, serjio_pd, "OOM Error");
		goto out;
	}
	spin_lock_irqsave(&rng->lock, flags);
	for (i = 0; i < rng->n_ents; i++) {
		union jblock_md *jmdc_entry = get_jmdc_entry(rng, i);
		const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_entry);
		if (j2d >= blkset_slba && j2d < blkset_slba + LOCKSET_4KS) {
			free_ents[num_ents].rng_idx = cpu_to_be16(rng->range_idx);
			free_ents[num_ents].rng_gen_id = cpu_to_be64(rng->gen_id);
			free_ents[num_ents].ent_idx = cpu_to_be16(i);
			free_ents[num_ents].ent_md = rng->jentry_md[i];
			num_ents++;
		}
	}
	spin_unlock_irqrestore(&rng->lock, flags);

	_NTs(trace_1_serjio_set_free_blkset_entries, serjio_pd, "Clearing Client @CLIENT_UUID_STR Range for LBA [@BLKSET_SLBA, @BLKSET_SLBA)",
		client_uuid_str, blkset_slba, blkset_slba + LOCKSET_4KS);
	if (!(rv = nvmeibs_serjio_free_jrnl_ents(
			serjio_pd->di, serjio_pd->boot_id, NULL,
			NVMEIB_RECOV_SRC_PROC_FILE, blkset_slba, num_ents, free_ents))) {
		rv = len;
	}

out:
	kfree(free_ents);
	return rv;
}

static ssize_t set_jmdc_entry(void *arg, char *buf, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	struct jrange_entry *jrng;
	u64 j2d;
	u32 range, tx_id, tx_bmp, ver;
	u32 entry;
	ssize_t rv = len;
	union jblock_md jmdc_entry;

	if (len < 3) {
		rv = -EINVAL;
		goto out;
	}

	if (sscanf(buf, "%u:%u:%llu:%u:%x:%x", &range, &entry, &j2d, &tx_id, &tx_bmp, &ver) != 6) {
		rv = -EINVAL;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		rv = -EAGAIN;
		goto out;
	}

	if (range >= serjio_pd->jranges_alloc_tbl.num_ranges) {
		_NE(error_1_serjio_set_jmdc_entry, "Invalid range: @JRNL_RNG_IDX", range);
		rv = -EINVAL;
		goto out;
	}

	if (ver != NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) {
		_NE(error_serjio_set_jmdc_entry_inv_ver,
				"Only version @VERSION supported. Use jmdc_set_ext proc interface",
				ver);
		rv = -EINVAL;
		goto out;
	}

	jrng = &serjio_pd->jranges_alloc_tbl.ranges[range];

	if (jrng->binje_shift != 0) {
		_NE(error_serjio_set_jmdc_entry_binje_not_zero,
				"Range @JRNL_RNG_IDX has N @BINJE > 0. Use jmdc_set_ext proc interface",
				range, 1 << jrng->binje_shift);
		rv = -EINVAL;
		goto out;
	}

	if (entry >= jrng->n_ents) {
		_NE(error_serjio_set_jmdc_entry, "Invalid entry: @JRNL_RNG_ENT_IDX", entry);
		rv = -EINVAL;
		goto out;
	}

	if (tx_id > ((1 << NVMEIB_EC_JMDC_BITS_TX_ID) - 1)) {
		_NE(error_2_serjio_set_jmdc_entry, "Invalid txid: @TXID", tx_id);
		rv = -EINVAL;
		goto out;
	}

	if (tx_bmp > ((1 << NVMEIB_EC_JMDC_BITS_TX_BMP) - 1)) {
		_NE(error_3_serjio_set_jmdc_entry, "Invalid tx_bmp @TXBM", tx_bmp);
		rv = -EINVAL;
		goto out;
	}

	jblock_md_jmd_encode_v0(&jmdc_entry, j2d, tx_id, tx_bmp);
	WARN_ON(ver != NVMEIBC_JOURNAL_MD_VERSION_UNPACKED);		// Not supported yet
	set_jmdc_entry_data(jrng, entry, &jmdc_entry);

	sync_jmdc_range_to_all_nics(serjio_pd, range);

out:
	return rv;
}

static ssize_t set_jmdc_ext(void *arg, char *buf, size_t len)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = arg;
	struct jrange_entry *jrng;
	u64 j2d_start, j2d_end;
	u32 range, tx_id, tx_bmp, ver;
	u32 entry;
	ssize_t rv = len;
	union jblock_md jmdc_entry[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY] = {};

	if (len < 3) {
		rv = -EINVAL;
		goto out;
	}

	if (sscanf(buf, "%u:%u:%llu:%llu:%u:%x:%u", &range, &entry, &j2d_start, &j2d_end, &tx_id, &tx_bmp, &ver) != 6) {
		rv = -EINVAL;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		rv = -EAGAIN;
		goto out;
	}

	if (range >= serjio_pd->jranges_alloc_tbl.num_ranges) {
		_NE(error_1_serjio_set_jmdc_ext_inv_rng, "Invalid range: @JRNL_RNG_IDX", range);
		rv = -EINVAL;
		goto out;
	}

	if (ver != NVMEIBC_JOURNAL_MD_VERSION_UNPACKED && ver != NVMEIBC_JOURNAL_MD_VERSION_PACKED) {
		_NE(error_serjio_set_jmdc_ext_inv_ver,
				"Invalid version @VERSION", ver);
		rv = -EINVAL;
		goto out;
	}

	jrng = &serjio_pd->jranges_alloc_tbl.ranges[range];

	if (entry >= jrng->n_ents) {
		_NE(error_serjio_set_jmdc_ext, "Invalid entry: @JRNL_RNG_ENT_IDX", entry);
		rv = -EINVAL;
		goto out;
	}

	if (ver == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED && jrng->binje_shift != 0) {
		_NE(err_serjio_set_jmdc_ext_inv_ver_2, "Invalid version for Range N @BINJE", 1 << jrng->binje_shift);
		rv = -EINVAL;
		goto out;
	}

	if ((j2d_start > j2d_end) || (j2d_end - j2d_start + 1) > (u64)(1 << jrng->binje_shift)) {
		_NE(error_serjio_set_jmdc_j2d, "Invalid J2D Range [@J2D_START,@J2D_END] for Range N @BINJE",
				j2d_start, j2d_end, 1 << jrng->binje_shift);
		rv = -EINVAL;
		goto out;
	}

	if (tx_id > ((1 << NVMEIB_EC_JMDC_BITS_TX_ID) - 1)) {
		_NE(error_2_serjio_set_jmdc_ext, "Invalid txid: @TXID", tx_id);
		rv = -EINVAL;
		goto out;
	}

	if (tx_bmp > ((1 << NVMEIB_EC_JMDC_BITS_TX_BMP) - 1)) {
		_NE(error_3_serjio_set_jmdc_ext, "Invalid tx_bmp @TXBM", tx_bmp);
		rv = -EINVAL;
		goto out;
	}

	if (ver == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) {
		jblock_md_jmd_encode_v0(&jmdc_entry[0], j2d_start, tx_id, tx_bmp);
	} else {
		unsigned i, j, tx_bmp_start, tx_bmp_end;
		ulong tx_bmp_long = tx_bmp;
		u32 tx_bmp_zip = 0;
		u16 val = 0;
		BUG_ON(ver != NVMEIBC_JOURNAL_MD_VERSION_PACKED);

		/* Calculate txbmp_zip: TBD: Use shared funcs when ported */
		val = 0;  // 0 is reserved value
		tx_bmp_start = find_first_bit(&tx_bmp_long, N_MAX_RAID_SLICE_LEN);
		tx_bmp_end = find_last_bit(&tx_bmp_long, N_MAX_RAID_SLICE_LEN);
		for (j = 0; j < N_MAX_RAID_SLICE_LEN; ++j) {
			for (i = 0; i <= j; i++) {
				if (i == tx_bmp_start && j == tx_bmp_end) {
					tx_bmp_zip = val;
					break;
				}
				val++;
			}
		}

		for (i = 0; j2d_start + i <= j2d_end; i++) {
			u64 j2d = j2d_start + i;
			jmdc_entry[i].j2d_0 = (u32)j2d;
			jmdc_entry[i].tx_id = tx_id;
			jmdc_entry[i].v1.tx_bmp_zip = tx_bmp_zip;
			jmdc_entry[i].v1.has_next = j2d < j2d_end ? 1 : 0;
			jmdc_entry[i].v1.j2d_extended = (j2d >> 32);
			jmdc_entry[i].version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
		}
	}

	set_jmdc_entry_data(jrng, entry, jmdc_entry);

	sync_jmdc_range_to_all_nics(serjio_pd, range);

out:
	return rv;
}

static void remove_disk_proc_files(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int i;
	for (i = 0; i < NVMEIB_EC_MAX_JOURNAL_RANGES; i++) {
		if (serjio_pd->jmdc_proc_files[i])
			nvmeib_public_proc_seq_remove(serjio_pd->jmdc_proc_files[i]);
		if (serjio_pd->rng_ents_proc_files[i])
			nvmeib_public_proc_seq_remove(serjio_pd->rng_ents_proc_files[i]);
	}
	if (serjio_pd->unknown_entry_proc_file)
		nvmeib_public_proc_remove(serjio_pd->unknown_entry_proc_file);
	if (serjio_pd->abnd_entry_proc_file)
		nvmeib_public_proc_remove(serjio_pd->abnd_entry_proc_file);
	if (serjio_pd->free_entry_proc_file)
		nvmeib_public_proc_remove(serjio_pd->free_entry_proc_file);
	if (serjio_pd->clients_csv_file)
		nvmeib_public_proc_remove(serjio_pd->clients_csv_file);
	if (serjio_pd->ranges_csv_file)
		nvmeib_public_proc_seq_remove(serjio_pd->ranges_csv_file);
	if (serjio_pd->jmdc_mapping_proc_file)
		nvmeib_public_proc_remove(serjio_pd->jmdc_mapping_proc_file);
	if (serjio_pd->jmdc_set_proc_file)
		nvmeib_public_proc_remove(serjio_pd->jmdc_set_proc_file);
	if (serjio_pd->jmdc_set_ext_proc_file)
		nvmeib_public_proc_remove(serjio_pd->jmdc_set_ext_proc_file);
	if (serjio_pd->parts_proc_file)
		nvmeib_public_proc_remove(serjio_pd->parts_proc_file);
	if (serjio_pd->free_blkset_entries_proc_file)
		nvmeib_public_proc_remove(serjio_pd->free_blkset_entries_proc_file);
	if (serjio_pd->boot_id_proc_file)
		nvmeib_public_proc_remove(serjio_pd->boot_id_proc_file);
	if (serjio_pd->serjio_stats_proc_file)
		nvmeib_public_proc_remove(serjio_pd->serjio_stats_proc_file);
	if (serjio_pd->serjio_stats_readable_proc_file)
		nvmeib_public_proc_remove(serjio_pd->serjio_stats_readable_proc_file);
	kfree(serjio_pd->jmdc_proc_files);
	kfree(serjio_pd->rng_ents_proc_files);
	serjio_pd->abnd_entry_proc_file = NULL;
	serjio_pd->free_entry_proc_file = NULL;
	serjio_pd->jmdc_mapping_proc_file = NULL;
	serjio_pd->jmdc_set_proc_file = NULL;
	serjio_pd->jmdc_set_ext_proc_file = NULL;
	serjio_pd->jmdc_proc_files = NULL;
	serjio_pd->parts_proc_file = NULL;
	serjio_pd->clients_csv_file = NULL;
	serjio_pd->ranges_csv_file = NULL;
	serjio_pd->free_blkset_entries_proc_file = NULL;
	serjio_pd->boot_id_proc_file = NULL;
	serjio_pd->serjio_stats_proc_file = NULL;
	serjio_pd->serjio_stats_readable_proc_file = NULL;
	if (serjio_pd->disk_proc_dir) {
		if (serjio_pd->jmdc_proc_dir)
			remove_proc_entry("jmdc", serjio_pd->disk_proc_dir);
		if (serjio_pd->jents_proc_dir)
			remove_proc_entry("jentries", serjio_pd->disk_proc_dir);
	}
	remove_proc_entry(nvmeibs_disk_info_get_disk_id(serjio_pd->di), serjio_proc_dir);
	if (atomic_dec_return(&num_serjio_disks) == 0) {
		remove_proc_entry("serjio", nvmeibs_proc_dir);
	}
}

static int create_disk_proc_files(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int rv = 0, i;
	NFIN;
	if (atomic_inc_return(&num_serjio_disks) == 1) {
		if (!(serjio_proc_dir = proc_mkdir("serjio", nvmeibs_proc_dir))) {
			_NE(error_serjio_create_disk_proc_files, "Failed to create serjio /proc dir");
			atomic_dec(&num_serjio_disks);
			rv = -EFAULT;
			goto out;
		}
	}
	if (!(serjio_pd->jmdc_proc_files = kzalloc(NVMEIB_EC_MAX_JOURNAL_RANGES *
			sizeof(*serjio_pd->jmdc_proc_files), GFP_KERNEL)) ||
		!(serjio_pd->rng_ents_proc_files = kzalloc(NVMEIB_EC_MAX_JOURNAL_RANGES *
			sizeof(*serjio_pd->jmdc_proc_files), GFP_KERNEL))) {
		_NEs(error_1_serjio_create_disk_proc_files, serjio_pd, "Failed to allocate memory for /proc files for disk @DISK_ID_STR",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		rv = -EFAULT;
		goto out;
	}
	if (!(serjio_pd->disk_proc_dir =
		proc_mkdir(nvmeibs_disk_info_get_disk_id(serjio_pd->di), serjio_proc_dir))) {
		_NEs(error_2_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc dir for disk @DISK_ID_STR",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->jmdc_proc_dir = proc_mkdir("jmdc", serjio_pd->disk_proc_dir))) {
		_NEs(error_3_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc jmdc dir for disk @DISK_ID_STR",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->jents_proc_dir = proc_mkdir("jentries", serjio_pd->disk_proc_dir))) {
		_NEs(create_disk_proc_files_e1, serjio_pd, "Failed to create /proc jentries dir for disk @STR",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->boot_id_proc_file = nvmeib_public_proc_create("boot_id",
		serjio_pd->disk_proc_dir, fill_boot_id, NULL, serjio_pd))) {
		_NEs(error_4_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file free_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->clients_csv_file = nvmeib_public_proc_create("clients.csv",
		serjio_pd->disk_proc_dir, fill_clients_csv, NULL, serjio_pd))) {
		_NEs(error_5_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file clients.csv");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->ranges_csv_file =
		nvmeib_public_proc_create_seq_data("ranges.csv", serjio_pd->disk_proc_dir,
									&fill_ranges_seq_ops, serjio_pd))) {
		_NEs(error_6_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file ranges.csv");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->jmdc_mapping_proc_file =
		nvmeib_public_proc_create("jmdc_mapping.csv", serjio_pd->disk_proc_dir,
			fill_jmdc_mapping_csv, NULL, serjio_pd))) {
		_NEs(error_7_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file jranges.csv");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->parts_proc_file =
		nvmeib_public_proc_create("partitions.csv", serjio_pd->disk_proc_dir,
				fill_parts_csv, NULL, serjio_pd))) {
		_NEs(error_8_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file partitions.csv");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->abnd_entry_proc_file =
		nvmeib_public_proc_create("abandon_entry", serjio_pd->disk_proc_dir,
				   NULL, set_jrange_abnd_entry, serjio_pd))) {
		_NEs(error_9_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file abandon_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->unknown_entry_proc_file =
		nvmeib_public_proc_create("unknown_entry", serjio_pd->disk_proc_dir,
				   NULL, set_jrange_unknown_entry, serjio_pd))) {
		_NEs(error_10_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file unknown_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->free_entry_proc_file =
		nvmeib_public_proc_create("free_entry", serjio_pd->disk_proc_dir,
				NULL, set_jrange_free_entry, serjio_pd))) {
		_NEs(error_11_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file free_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->jmdc_set_proc_file =
		nvmeib_public_proc_create("jmdc_set", serjio_pd->disk_proc_dir,
				NULL, set_jmdc_entry, serjio_pd))) {
		_NEs(error_12_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file free_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->jmdc_set_ext_proc_file =
		nvmeib_public_proc_create("jmdc_set_ext", serjio_pd->disk_proc_dir,
				NULL, set_jmdc_ext, serjio_pd))) {
		_NEs(error_15_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file free_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->free_blkset_entries_proc_file =
		nvmeib_public_proc_create("free_blkset_entries", serjio_pd->disk_proc_dir,
				NULL, set_free_blkset_entries, serjio_pd))) {
		_NEs(error_13_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file free_entry");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->serjio_stats_proc_file =
		nvmeib_public_proc_create("stats.json", serjio_pd->disk_proc_dir,
				fill_serjio_stats, stats_clear, serjio_pd))) {
		_NEs(error_16_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file stats.json");
		rv = -EFAULT;
		goto remove_proc;
	}
	if (!(serjio_pd->serjio_stats_readable_proc_file =
		nvmeib_public_proc_create("stats", serjio_pd->disk_proc_dir,
				fill_serjio_stats_readable, stats_clear, serjio_pd))) {
		_NEs(error_17_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file stats");
		rv = -EFAULT;
		goto remove_proc;
	}
	for (i = 0; i < NVMEIB_EC_MAX_JOURNAL_RANGES; i++) {
		char proc_fname[32];
		sprintf(proc_fname, "%04d.csv", i);
		if (!(serjio_pd->jmdc_proc_files[i] =
			nvmeib_public_proc_create_seq_data(proc_fname, serjio_pd->jmdc_proc_dir,
							   &fill_jmdc_seq_ops, &serjio_pd->jranges_alloc_tbl.ranges[i]))) {
			_NEs(error_14_serjio_create_disk_proc_files, serjio_pd, "Failed to create /proc file @PROC_FNAME for jmdc", proc_fname);
			rv = -EFAULT;
			goto remove_proc;
		}
		sprintf(proc_fname, "rng%04d.csv", i);
		if (!(serjio_pd->rng_ents_proc_files[i] =
			nvmeib_public_proc_create_seq_data(proc_fname, serjio_pd->jents_proc_dir,
							   &fill_jents_csv_seq_ops, &serjio_pd->jranges_alloc_tbl.ranges[i]))) {
			_NEs(create_disk_proc_files_e2, serjio_pd, "Failed to create /proc file @STR for range @INT entries", proc_fname, i);
			rv = -EFAULT;
			goto remove_proc;
		}
	}

	goto out;

remove_proc:
	remove_disk_proc_files(serjio_pd);
out:
	if (rv) {
		if (atomic_dec_return(&num_serjio_disks) == 0) {
			if (serjio_proc_dir) {
				remove_proc_entry("serjio", nvmeibs_proc_dir);
				serjio_proc_dir = NULL;
			}
		}
	}
	NFOUT;
	return rv;
}

static int alloc_jmdc(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	int rv;
	int i;
	size_t alloc_size = 0;

	NFIN;
#if !ALLOC_JMDC_AFTER_RD_GPT
	serjio_pd->jmdc_mem.len = (NVMEIB_EC_MAX_JOURNAL_RANGES << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT) * sizeof(union jblock_md);
#else
	serjio_pd->jmdc_mem.len = DISK_LBAS_TO_JOURNAL_BLKS(serjio_pd->di, serjio_pd->disk_ranges.journal.len_nlbas) * sizeof(union jblock_md);
#endif

	serjio_pd->jmdc_mem.n_pages = DIV_ROUND_UP(serjio_pd->jmdc_mem.len, PAGE_SIZE);
	_NTs(trace_serjio_alloc_jmdc, serjio_pd, "jmdc memory n_pages=@N_PAGES", serjio_pd->jmdc_mem.n_pages);

	serjio_pd->jmdc_mem.pages = kcalloc(serjio_pd->jmdc_mem.n_pages, sizeof(*serjio_pd->jmdc_mem.pages), GFP_KERNEL);
	if (!serjio_pd->jmdc_mem.pages) {
		_NEs(error_1_serjio_alloc_jmdc, serjio_pd, "JMDC memory allocation error for disk @DISK_ID_STR",
			nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		rv = -ENOMEM;
		goto out;
	}
	for (i = 0; i < (int)serjio_pd->jmdc_mem.n_pages; ++i) {
		serjio_pd->jmdc_mem.pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (serjio_pd->jmdc_mem.pages[i] == NULL) {
			_NEs(error_2_serjio_alloc_jmdc, serjio_pd, "could not allocate JMDC memory");
			rv = -ENOMEM;
			goto out;
		}
	}
	nvmeib_ref_init(&serjio_pd->jmdc_mem.refcount);
	alloc_size = calc_jmdc_mem_alloc_sz(serjio_pd);
	rv = 0;
out:
	nvmesh_memmgr_metric_on_alloc_update(serjio_jmdc_mem, alloc_size, (rv == 0));
	NFOUT;
	return rv;
}

TIMER_CALLBACK(wq_pend_timer_cb, struct nvmeibs_serjio_disk_private_data, wq_pend_timer, struct nvmeibs_serjio_disk_private_data, serjio_pd)
	struct run_iowq_workqe *run_workqe, *t;
	unsigned long cur_jif = jiffies;
	unsigned long next_timer_jif = 0;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->io_wq_pend_lock, flags);
	list_for_each_entry_safe(run_workqe, t, &serjio_pd->io_wq_pend_list, pend_link) {
		if (!run_workqe->comp)
			continue;
		if (run_workqe->pend_jif + SERJIO_WQ_PEND_MAX_WAIT > cur_jif)
			continue;
		list_del(&run_workqe->pend_link);
		BUG_ON(serjio_pd->io_wq_pend_cnt == 0);
		serjio_pd->io_wq_pend_cnt--;

		_NTs(trace_serjio_check_io_pending_timeout, serjio_pd, "Pending work @PTR for fn @FN timed-out after @JIFFIES\n",
		run_workqe, run_workqe->fn, cur_jif - run_workqe->pend_jif);
		if (run_workqe->rv)
			*run_workqe->rv = -ETIMEDOUT;
		complete(run_workqe->comp);
		kfree(run_workqe);
	}
	if ((run_workqe = list_first_entry_or_null(
		&serjio_pd->io_wq_pend_list, typeof(*run_workqe), pend_link))) {
		next_timer_jif = run_workqe->pend_jif + SERJIO_WQ_PEND_MAX_WAIT;
	}
	spin_unlock_irqrestore(&serjio_pd->io_wq_pend_lock, flags);

	if (!IS_SERJIO_DYING(serjio_pd) && next_timer_jif)
		mod_timer(&serjio_pd->wq_pend_timer, next_timer_jif);
}

static void drain_io_pending(struct nvmeibs_serjio_disk_private_data *serjio_pd, bool move_to_wq)
{
	struct run_iowq_workqe *run_workqe;
	unsigned long flags;
	spin_lock_irqsave(&serjio_pd->io_wq_pend_lock, flags);
	while ((run_workqe = list_first_entry_or_null(
			&serjio_pd->io_wq_pend_list, typeof(*run_workqe), pend_link))) {
		list_del(&run_workqe->pend_link);
		BUG_ON(serjio_pd->io_wq_pend_cnt == 0);
		serjio_pd->io_wq_pend_cnt--;
		if (move_to_wq && atomic_inc_not_zero(&serjio_pd->io_wq_cnt)) {
			_NTs(trace_serjio_2169, serjio_pd, "Moving pending work @PTR for fn @FN to WQ\n",
				 run_workqe, run_workqe->fn);
			BUG_ON(!wq_add_work(serjio_pd->io_wq, &run_workqe->work));
		} else {
			_NTs(trace_serjio_2173, serjio_pd, "Cancelling pending work @PTR for fn @FN\n",
				 run_workqe, run_workqe->fn);
			if (run_workqe->rv)
				*run_workqe->rv = -ECANCELED;
			if (run_workqe->comp)
				complete(run_workqe->comp);
			kfree(run_workqe);
		}
	}
	spin_unlock_irqrestore(&serjio_pd->io_wq_pend_lock, flags);
}

static void add_to_wq_pend(struct run_iowq_workqe *run_workqe)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = run_workqe->serjio_pd;
	unsigned long pend_timeout_jif;
	unsigned long flags;
	enum nvmeibs_serjio_state srj_state;

	/* Put in pending list for when SERJIO changes state */
	spin_lock_irqsave(&serjio_pd->io_wq_pend_lock, flags);
	list_add_tail(&run_workqe->pend_link, &serjio_pd->io_wq_pend_list);
	serjio_pd->io_wq_pend_cnt++;

	run_workqe->pend_jif = jiffies;
	pend_timeout_jif = run_workqe->pend_jif + SERJIO_WQ_PEND_MAX_WAIT;

	srj_state = serjio_state_get(serjio_pd);
	_NTs(t1_run_on_io_wq_workfn, serjio_pd,
		"adding @FN to pending-list (cnt=@COUNT), "
		"state: curr=@SERJIO_STATE",
		run_workqe->fn, serjio_pd->io_wq_pend_cnt, srj_state);
	spin_unlock_irqrestore(&serjio_pd->io_wq_pend_lock, flags);

	/* Set a timer in case SERJIO takes too long to change state */
	if (!timer_pending(&serjio_pd->wq_pend_timer))
		mod_timer(&serjio_pd->wq_pend_timer, pend_timeout_jif);
}

static void run_on_io_wq_workfn(struct workqe_struct *work_qe)
{
	struct run_iowq_workqe *run_workqe =
		container_of(work_qe, struct run_iowq_workqe, work);
	struct nvmeibs_serjio_disk_private_data *serjio_pd = run_workqe->serjio_pd;
	int rv;
	enum nvmeibs_serjio_state prev_srj_state = serjio_state_get(serjio_pd), post_srj_state;
	bool resched = false;

	rv = __run_on_io_wq_fn(false, (run_workqe->fn), serjio_pd, (run_workqe->param), &resched, &run_workqe->work_stats);
	if (rv < 0 && rv != -EINPROGRESS && rv != -EAGAIN)
		_NE(error_serjio_run_on_io_wq_workfn, "@FN failed (@RV)", run_workqe->fn, rv);

	if (atomic_dec_return(&serjio_pd->io_wq_cnt) == 0)
		complete(&serjio_pd->io_wq_cmp);
	else if (resched) {
		BUG_ON(rv != -EAGAIN);
		add_to_wq_pend(run_workqe);
		return;
	}

	if (run_workqe->rv)
		*run_workqe->rv = rv;
	if (run_workqe->comp)
		complete(run_workqe->comp);
	kfree(run_workqe);

	if ((post_srj_state = serjio_state_get(serjio_pd)) != prev_srj_state &&
			post_srj_state != SERJIO_DYING) {
		/* SERJIO state changed (and not DYING) => Re-run Pending Work */
		_NTs(trace_nvmeibs_serjio_c_2191, serjio_pd,
			"SERJIO state changed @SERJIO_STATE => @SERJIO_STATE. Rescheduling @COUNT pending work items",
			prev_srj_state, post_srj_state, serjio_pd->io_wq_pend_cnt);

		drain_io_pending(serjio_pd, true);
	}
}

static int run_on_io_wq(struct nvmeibs_serjio_disk_private_data *serjio_pd,
						io_wq_fn_type fn, void *param,
						bool wait_complete, bool can_sleep, enum nvmeibs_serjio_work_type work_type)
{
	struct run_iowq_workqe *work = NULL;
	int rv = 0;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct nvmeibs_serjio_work_stats work_stats = {0, 0, 0, 0, false, work_type};

	NFIN;

	_NTs(t0_serjio_run_on_io_wq, serjio_pd,
		 "--> attempt work-fn '@FN' (from '@__BUILTIN_RETURN_ADDRESS_FUNC' on wq=@PID)...",
		 fn, __builtin_return_address(0), serjio_pd->io_wq_pid);

	if (IS_SERJIO_DYING(serjio_pd)) {
		_NTs(e0_serjio_run_on_io_wq, serjio_pd, "Serjio is dying");
		rv = -EBUSY;
		goto out;
	}
	nvmeibs_serjio_update_work_stats_queued(&work_stats, &serjio_pd->stats);
	if (on_wq_pid(serjio_pd->io_wq_pid) && wait_complete) {
		bool resched = false;
		/* We are currently on the WQs pid and waiting for completion
		- just run the fn directly */
		rv = __run_on_io_wq_fn(true, fn, serjio_pd, param, &resched, &work_stats);
		BUG_ON(resched); /* We can't reschedule b/c we are waiting from the same ctx */
		goto out;
	}
	if (!(work = kzalloc(sizeof(*work), (!can_sleep ? GFP_ATOMIC : GFP_KERNEL)))) {
		_NEs(e1_serjio_run_on_io_wq, serjio_pd, "Memory Allocation Error");
		rv = -ENOMEM;
		goto out;
	}
	work->serjio_pd = serjio_pd;
	work->fn = fn;
	work->param = param;
	work->work_stats = work_stats;
	if (wait_complete) {
		work->comp = &comp;
		work->rv = &rv;
	}
	WQ_INIT_WORK(&work->work, run_on_io_wq_workfn);
	if (atomic_inc_not_zero(&serjio_pd->io_wq_cnt)) {
		if (!wq_add_work(serjio_pd->io_wq, &work->work)) {
			if (atomic_dec_return(&serjio_pd->io_wq_cnt) == 0) {
				complete(&serjio_pd->io_wq_cmp);
			}
			_NEs(e2_serjio_run_on_io_wq, serjio_pd, "Already on WQ");
			kfree(work);
			rv = -EALREADY;
			goto out;
		}
	} else {
		kfree(work);
		rv = -EBUSY;
		goto out;
	}

	if (wait_complete)
		wait_for_completion(&comp);
	else
		rv = -EINPROGRESS;

out:
	_NTs(t2_serjio_run_on_io_wq, serjio_pd,
		 "<-- work-fn '@FN', rv=@RV", fn, rv);

	NFOUT;
	return rv;
}

TIMER_CALLBACK(jgc_timer_cb, struct nvmeibs_serjio_disk_private_data, jgc_timer, struct nvmeibs_serjio_disk_private_data, serjio_pd)

	run_on_io_wq(
		serjio_pd, io_chk_jgc_fn, NULL, false, false, NVMEIBS_SERJIO_WORK_TYPE_CHK_JGC);
}

/* allocate EC JMDC used by recovery process */
int nvmeibs_serjio_disk_init(struct nvmeibs_disk_info *di)
{
	int ret = -ENODEV;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	u8 uuid[16];

	NFIN;
	BUILD_BUG_ON(NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT > PAGE_SHIFT);
	BUILD_BUG_ON(NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT < NVMEIBC_SECTOR_SHIFT);
	BUILD_BUG_ON(
		sizeof(struct serjio_db_jrange_entry) >
		(1 << NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT));

	if ((unsigned)(1 << ilog2(nvmeibs_jrange_num_blocks)) != nvmeibs_jrange_num_blocks ||
		nvmeibs_jrange_num_blocks < NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3 ||
		nvmeibs_jrange_num_blocks > NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE)
	{
		_NE(error_serjio_nvmeibs_serjio_disk_init_inv_jrng_n_blk,
		    "SERJIO FATAL: Invalid value for modparam nvmeibs_jrange_num_blocks: @N_BLOCKS ",
		    nvmeibs_jrange_num_blocks);
		ret = -EINVAL;
		goto out;
	}

	if (!nvmeibs_disk_info_is_ready_for_serjio(di) ) {
		_NE(error_serjio_nvmeibs_serjio_disk_init, "SERJIO FATAL: empty disk info of disk @DISK_ID_STR", nvmeibs_disk_info_get_disk_id(di));
		goto out;
	}
#ifndef BLKDEV_SIMULATOR
	/* Not relevant for simulator */
	if (nvmeibs_disk_info_has_mtdt_extd(di)) {
		_NE(error_serjio_nvmeibs_serjio_disk_init_md_extd, "SERJIO FATAL: disk @DISK_ID_STR has extended MD which is not supported",
		    nvmeibs_disk_info_get_disk_id(di));
		ret = -ENOTSUPP;
		goto out;
	}
	if (di->block_size != NVMEIBC_SECTOR_SIZE) {
		_NE(error_serjio_nvmeibs_serjio_disk_init_block_size, "SERJIO FATAL: disk @DISK_ID_STR has block size @BLOCK_SIZE which is not supported",
		    nvmeibs_disk_info_get_disk_id(di), di->block_size);
		ret = -ENOTSUPP;
		goto out;
	}
#endif
	/* Init serjio private data struct */
	if (!(serjio_pd = kzalloc(sizeof(*serjio_pd), GFP_KERNEL))) {
		_NE(error_1_serjio_nvmeibs_serjio_disk_init, "SERJIO FATAL: Memory Allocation Error");
		ret = -ENOMEM;
		goto out;
	}
	serjio_pd->di = di;
	serjio_pd->rng_nblk = nvmeibs_jrange_num_blocks;
	serjio_pd->rng_nlba = JOURNAL_BLKS_TO_DISK_LBAS(di, nvmeibs_jrange_num_blocks);
	spin_lock_init(&serjio_pd->lock);
	init_rwsem(&serjio_pd->gpt_rwsem);
	nvmeibs_serjio_uuid_generate(uuid);
	nvmeibs_serjio_uuid_to_str(serjio_pd->boot_id, uuid);

	if (!(serjio_pd->io_wq = nvmeibs_serjio_wq_create()))
		goto free_pd;
	serjio_pd->io_wq_pid = wq_pid(serjio_pd->io_wq);
	atomic_set(&serjio_pd->io_wq_cnt, 1);
	init_completion(&serjio_pd->io_wq_cmp);
	INIT_LIST_HEAD(&serjio_pd->io_wq_pend_list);
	spin_lock_init(&serjio_pd->io_wq_pend_lock);

	INIT_LIST_HEAD(&serjio_pd->jmdc_mems);
	spin_lock_init(&serjio_pd->jmdc_mem_lock);

	/* Setup timer for JGC checking */
	SETUP_TIMER(&serjio_pd->jgc_timer, jgc_timer_cb,
				(unsigned long)serjio_pd, 0);
	TIMER_SET_DATA(serjio_pd, jgc_timer, (unsigned long) serjio_pd);

	/* Setup timer for WQ Pending Timeout */
	SETUP_TIMER(&serjio_pd->wq_pend_timer, wq_pend_timer_cb,
		    (unsigned long)serjio_pd, 0);
	TIMER_SET_DATA(serjio_pd, wq_pend_timer, (unsigned long) serjio_pd);

	/* Init NVME Op Resources */
	if ((ret = init_nvme_op_rsrc(serjio_pd)))
		goto destroy_wq;

	/* Init journal range lists */
	if ((ret = init_jrange_lists(serjio_pd, true))) {
		goto free_nvme_rsrc;
	}

#if !ALLOC_JMDC_AFTER_RD_GPT
	/* Allocate JMDC required memory */
	if ((ret = alloc_jmdc(serjio_pd))) {
		goto free_jranges;
	}
#endif

	/* Create /proc Files */
	if ((ret = create_disk_proc_files(serjio_pd)))
#if !ALLOC_JMDC_AFTER_RD_GPT
		goto free_jmdc;
#else
		goto free_jranges;
#endif

	serjio_pd->state = SERJIO_INIT;
	nvmeibs_serjio_stats_init(&serjio_pd->stats);
	nvmeibs_serjio_state_init(&serjio_pd->stats, SERJIO_INIT);
	spin_lock_init(&serjio_pd->state_lock);
#if KS_RB_ROOT_CACHED
	serjio_pd->jrnl_seg_rb_root = RB_ROOT_CACHED;
	serjio_pd->del_seg_rb_root = RB_ROOT_CACHED;
#else
	serjio_pd->jrnl_seg_rb_root = RB_ROOT;
	serjio_pd->del_seg_rb_root = RB_ROOT;
#endif
	INIT_RADIX_TREE(&serjio_pd->jrnl_seg_radix_root, GFP_KERNEL);

	spin_lock_init(&serjio_pd->cln_seg_list_lock);
	INIT_LIST_HEAD(&serjio_pd->cln_seg_wait_scan_list);
	INIT_LIST_HEAD(&serjio_pd->cln_seg_scan_list);
	INIT_LIST_HEAD(&serjio_pd->cln_seg_wait_ret_list);

	if ((ret = run_on_io_wq(serjio_pd, io_rd_gpt_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_GPT)) != -EINPROGRESS) {
		goto remove_proc;
	}

	/* No error: Set SERJIO private data into disk struct */
	nvmeibs_disk_info_set_serjio_private_data(di, serjio_pd);

	_NTs(trace_1_serjio_nvmeibs_serjio_disk_init, serjio_pd, "SERJIO Init\n");
	ret = 0;
	goto out;

remove_proc:
	remove_disk_proc_files(serjio_pd);

#if !ALLOC_JMDC_AFTER_RD_GPT
free_jmdc:
	free_jmdc_memory(serjio_pd);
#endif

free_jranges:
	vfree(serjio_pd->jranges_alloc_tbl.ranges);
	nvmesh_memmgr_metric_on_free_update(serjio_entry_table, (NVMEIB_EC_MAX_JOURNAL_RANGES * sizeof(struct jrange_entry)));

free_nvme_rsrc:
	free_nvme_op_rsrc(serjio_pd);

destroy_wq:
	nvmeibs_serjio_wq_destroy(serjio_pd->io_wq);

free_pd:
	kfree(serjio_pd);

out:
	NFOUT;
	return ret;
}

static void jmdc_unmap_n_free(struct jmdc_mapping* jmdc_map, bool release_start)
{
	if (release_start)
		nvmeib_ref_release_start(&jmdc_map->refcount);
	nvmeib_ref_release_wait(&jmdc_map->refcount);
	nvmeibs_serjio_unmapn_n_free(&jmdc_map->jmdc_area_map);
	nvmeib_ref_put(&jmdc_map->serjio_pd->jmdc_mem.refcount);
	kfree(jmdc_map);
	nvmesh_memmgr_metric_on_free_update(serjio_jmdc_map, sizeof(*jmdc_map));
}

static void jmdc_unmap(struct nvmeibs_serjio_disk_private_data *serjio_pd,
					   struct nvmeibs_dev *nic)
{
	struct jmdc_mapping* jmdc = 0;
	int found_nic = 0;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_for_each_entry(jmdc, &serjio_pd->jmdc_mems, link) {
		if (jmdc->nic_dev == nic) {
			_NTs(trace_serjio_jmdc_unmap, serjio_pd, "found nic to unmap jmdc from @NIC @SERJIO_PD", nic, serjio_pd);
			found_nic = 1;
			break;
		}
	}
	if (!found_nic) {
		_NEs(error_serjio_jmdc_unmap, serjio_pd, "failed finding nic @NIC @SERJIO_PD", nic, serjio_pd);
		spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);
		goto out;
	}

	list_del(&jmdc->link);
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);
	jmdc_unmap_n_free(jmdc, true);

out:
	NFOUT;
}

static void jmdc_unmap_all(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	struct jmdc_mapping* jmdc;
	unsigned long flags;
	LIST_HEAD(unmap_list);

	NFIN;
	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	/* Start release on all mappings */
	while ((jmdc = list_first_entry_or_null(&serjio_pd->jmdc_mems, struct jmdc_mapping, link))) {
		nvmeib_ref_release_start(&jmdc->refcount);
		list_del(&jmdc->link);
		list_add_tail(&jmdc->link, &unmap_list);
	}
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);

	/* Wait for and free all mappings */
	while ((jmdc = list_first_entry_or_null(&unmap_list, struct jmdc_mapping, link))) {
		_NTs(jmdc_unmap_all_t1, serjio_pd, "calling jmdc_unmap_n_free()  serjio_pd @PTR jmdc @PTR jmdc map @PTR",
		     serjio_pd, jmdc, &jmdc->jmdc_area_map);
		list_del(&jmdc->link);
		jmdc_unmap_n_free(jmdc, false);
	}

	NFOUT;
}

void nvmeibs_serjio_unmap_jmd_cache_on_dev(struct nvmeibs_dev *nic)
{
	struct nvmeibs_disk_info *di;
	struct list_head *disks = nvmeibs_serjio_get_disks(NULL);

	NFIN;
	list_for_each_entry(di, disks, link) {
		struct nvmeibs_serjio_disk_private_data *serjio_pd =
			nvmeibs_disk_info_get_serjio_private_data(di);
		if (serjio_pd)
			jmdc_unmap(serjio_pd, nic);
	}

	nvmeibs_serjio_put_disks();
	NFOUT;
}

int nvmeibs_serjio_set_journal_info(struct nvmeibs_disk_info *di,
				     struct nvmeibs_toma_journal_msg* journal_msg)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	int rv = 0;

	NFIN;
	(void)journal_msg;
	if (!serjio_pd) {
		/* SERJIO failed to initialize */
		_NE(error_1_serjio_nvmeibs_serjio_set_journal_info, "Disk @DISK_ID_STR has no SERJIO private data", nvmeibs_disk_info_get_disk_id(di));
		rv = -ENOMEM;
		goto out;
	}
	_NTs(nvmeibs_serjio_set_journal_info_t1, serjio_pd, "NVMEIBS_TOMA_JOURNAL_INFO - Deprecated!");

out:
	NFOUT;
	return rv;
}

void nvmeibs_serjio_disk_free(struct nvmeibs_disk_info *di)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct nvme_op_rsrc_pool *nvme_op_rsrc_pool = &serjio_pd->nvme_op_rsrc_pool;
	int rng;

	if (!nvmeibs_disk_info_is_ready_for_serjio(di)){
		return;
	}

	if (!serjio_pd) {
		_NT(trace_serjio_nvmeibs_serjio_disk_free, "SERJIO: Disk has NULL serjio_pd");
		return;
	}

	nvmeibs_disk_info_set_serjio_private_data(di, NULL);

	_NTs(trace_nvmeibs_serjio_c_2476, serjio_pd, "SERJIO Free");

	SET_SERJIO_DYING(serjio_pd);

	del_timer_sync(&serjio_pd->jgc_timer);

	del_timer_sync(&serjio_pd->wq_pend_timer);

	if (atomic_dec_return(&serjio_pd->io_wq_cnt))
		wait_for_completion(&serjio_pd->io_wq_cmp);
	wq_drain(serjio_pd->io_wq);

	nvmeibs_serjio_wq_destroy(serjio_pd->io_wq);
	serjio_pd->io_wq = NULL;

	/* Drain pending io wq */
	drain_io_pending(serjio_pd, false);

	/* Warn if any journal ranges are still allocated */
	for (rng = 0; rng < (int)serjio_pd->jranges_alloc_tbl.num_ranges; rng++) {
		if (serjio_pd->jranges_alloc_tbl.ranges[rng].status == JRANGE_ALLOCATED) {
			_NW(warn_serjio_nvmeibs_serjio_disk_free, "Journal range @RNG is still allocated to client", rng);
		}
	}

	/* Warn if any IO is outstanding */
	if (nvme_op_rsrc_pool->n_free < NUM_OF_NVME_OP_RSRC) {
		_NW(warn_1_serjio_nvmeibs_serjio_disk_free, "@N_FREE Outstanding IO", NUM_OF_NVME_OP_RSRC - nvme_op_rsrc_pool->n_free);
	}

	remove_disk_proc_files(serjio_pd);

	vfree(serjio_pd->jranges_alloc_tbl.ranges);
	serjio_pd->jranges_alloc_tbl.ranges = NULL;
	nvmesh_memmgr_metric_on_free_update(serjio_entry_table, (NVMEIB_EC_MAX_JOURNAL_RANGES * sizeof(struct jrange_entry)));

	vfree(serjio_pd->gpt_entry);
	serjio_pd->gpt_entry = NULL;

	vfree(serjio_pd->pend_gpt_ents);
	serjio_pd->pend_gpt_ents = NULL;

	kfree(serjio_pd->pend_gpt_hdr);
	serjio_pd->pend_gpt_hdr = NULL;

	free_nvme_op_rsrc(serjio_pd);

	_NTs(trace_1_serjio_nvmeibs_serjio_disk_free, serjio_pd, "unmapping all jmdc's   @SERJIO_PD", serjio_pd);
	jmdc_unmap_all(serjio_pd);

	_NTs(trace_2_serjio_nvmeibs_serjio_disk_free, serjio_pd, "Freeing jmdc memory");
	free_jmdc_memory(serjio_pd);

	clr_seg_tree_hash(serjio_pd);

	_NTs(trace_3_serjio_nvmeibs_serjio_disk_free, serjio_pd, "Freeing serjio private data");
	kfree(serjio_pd);
}

enum nvmeibs_serjio_status nvmeibs_serjio_get_status(struct nvmeibs_disk_info *di)
{
	enum nvmeibs_serjio_status rv = NVMEIBS_SERJIO_STATUS_NOT_FOUND;
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	enum nvmeibs_serjio_state curr_state;

	if (!nvmeibs_disk_info_get_md_size(di)) {
		rv = NVMEIBS_SERJIO_STATUS_NO_MD;
		goto out;
	}
#ifndef BLKDEV_SIMULATOR
	/* Not relevant for simulator */
	if (nvmeibs_disk_info_has_mtdt_extd(di) || di->block_size != NVMEIBC_SECTOR_SIZE) {
		rv = NVMEIBS_SERJIO_STATUS_NOT_SUPP;
		goto out;
	}
#endif

	if (!serjio_pd) {
		_NT(trace_serjio_nvmeibs_serjio_get_state, "SERJIO: Disk has NULL serjio_pd");
		goto out;
	}

	curr_state = serjio_state_get(serjio_pd);

	if (SERJIO_ERROR(curr_state)) {
		if (curr_state == SERJIO_ERR_GPT)
			rv = NVMEIBS_SERJIO_STATUS_NO_GPT;
		else if (curr_state == SERJIO_ERR_NO_JRNL)
			rv = NVMEIBS_SERJIO_STATUS_NO_JOURNAL;
		else if (curr_state == SERJIO_ERR_NO_DB)
			rv = NVMEIBS_SERJIO_STATUS_NO_DB;
		else
			rv = NVMEIBS_SERJIO_STATUS_ERROR;
	} else if (SERJIO_RUNNING(curr_state))
		rv = NVMEIBS_SERJIO_STATUS_READY;
	else
		rv = NVMEIBS_SERJIO_STATUS_NOT_READY;

out:
	return rv;
}

ssize_t fill_serjios(void *dummy, char *buffer, size_t len)
{
	ssize_t count = 0;
	int n_disks;
	struct list_head *disks;
	struct nvmeibs_disk_info *di;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;

	NFIN;
	(void)dummy;
	count += scnprintf(buffer + count, len - count, NVMEIBS_SERJIOS_CSV_HEADER_EOL);
	disks = nvmeibs_serjio_get_disks(&n_disks);
	list_for_each_entry(di, disks, link) {
		if ((serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di))) {
			count += scnprintf(buffer + count, len - count,
				"%s,%s,%d,%d,%llu,%llu,%llu,%llu,%d,%d\n",
				nvmeibs_disk_info_get_disk_id(di),
				nvmeib_shared_serjio_status_to_str(nvmeibs_serjio_get_status(di)),
				serjio_pd->jranges_alloc_tbl.num_valid_rngs,
				serjio_pd->jranges_alloc_tbl.num_free_rngs,
				serjio_pd->disk_ranges.journal.lba,
				serjio_pd->disk_ranges.journal.len_nlbas,
				serjio_pd->disk_ranges.db.lba,
				serjio_pd->disk_ranges.db.len_nlbas,
				NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
				serjio_pd->rng_nblk);
		} else {
			count += scnprintf(buffer + count, len - count,
				"%s,%s,0,0,0,0,0,0,0,0\n",
				nvmeibs_disk_info_get_disk_id(di),
				nvmeib_shared_serjio_status_to_str(nvmeibs_serjio_get_status(di)));
		}
	}
	nvmeibs_serjio_put_disks();

	NFOUT;
	return count;
}

static void calc_io_completion_stats_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	enum nvmeibs_serjio_work_type current_work_type = serjio_pd->current_work_type;
	bool is_error = status != 0;
	bool is_read = op_rsrc->nvme_req.nvme_op == nvme_cmd_read;
	size_t comp_bytes = op_rsrc->nvme_req.data_len;
	size_t comp_lbas = op_rsrc->nvme_req.data_len >> nvmeibs_disk_info_get_block_shift(serjio_pd->di);
	int comp_iops = 1;

	/* Update your stats here with current_work_type, is_error, is_read, comp_bytes, comp_lbas and comp_iops  
	 * Make sure to take spinlock as the completions can come on multiple NVME queues */

	nvmeibs_serjio_update_io_stats_on_rsrc_completion(&serjio_pd->stats, current_work_type, comp_bytes, comp_lbas, comp_iops, is_read, is_error, &op_rsrc->op_rsrc_stats);

	/* Call the original callback */
	(*op_rsrc->serjio_cb)(arg, status, result);
}


static void init_nvme_op_rsrc_io(struct nvme_op_rsrc *rsrc,
		struct nvmeibs_serjio_disk_private_data *serjio_pd, int nvme_cmd,
		bool hw_blocks, sector_t disk_block, size_t data_len, nvme_callback_t *cb, void *cb_param,
		const void *md, size_t md_sz)
{
	struct nvmeibs_nvme_req *nvme_req = &rsrc->nvme_req;

	rsrc->serjio_cb = cb;
	rsrc->status = 0;

	nvme_req->nvme_op = nvme_cmd;
	nvme_req->use_hw_blocks = hw_blocks ? 1 : 0;
	nvme_req->disk_block = disk_block;
	nvme_req->data_len = data_len;
	nvme_req->cb = calc_io_completion_stats_cb;
	rsrc->param = cb_param;
	nvme_req->arg = rsrc;
	nvme_req->disk_info = serjio_pd->di;

	if (hw_blocks) {
		BUG_ON((data_len & ((1 << nvmeibs_disk_info_get_block_shift(serjio_pd->di)) - 1)) != 0);
	} else {
		BUG_ON((data_len & (NVMEIBC_SECTOR_SIZE - 1)) != 0);
	}
	if (nvme_cmd == nvme_cmd_read || nvme_cmd == nvme_cmd_write)
		BUG_ON(data_len > rsrc->n_data_pgs << PAGE_SHIFT);

	set_nvme_op_rsrc_md(rsrc, md, md_sz);
}

static struct nvme_op_rsrc *get_free_nvme_op_rsrc_sync(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, int max_attempts, bool can_schedule)
{
	struct nvme_op_rsrc *rsrc = NULL;
	struct nvme_op_rsrc_pool *nvme_op_rsrc_pool = &serjio_pd->nvme_op_rsrc_pool;
	unsigned long flags;
	int i = 0;

	spin_lock_irqsave(&nvme_op_rsrc_pool->lock, flags);
	while (!(rsrc = list_first_entry_or_null(&nvme_op_rsrc_pool->free_list,
											struct nvme_op_rsrc, link))) {
		if (can_schedule)
			nvme_op_rsrc_pool->n_waiters++;
		spin_unlock_irqrestore(&nvme_op_rsrc_pool->lock, flags);
		if (!can_schedule && i++ >= max_attempts)
			goto out;
		if (can_schedule)
			wait_for_completion_interruptible(&nvme_op_rsrc_pool->free_comp);
		else
			cpu_relax();
		if (IS_SERJIO_DYING(serjio_pd))
			goto out;
		spin_lock_irqsave(&nvme_op_rsrc_pool->lock, flags);
		if (can_schedule) {
			if (--nvme_op_rsrc_pool->n_waiters == 0)
				nvmeib_reinit_completion(&nvme_op_rsrc_pool->free_comp);
		}
	}
	list_del_init(&rsrc->link);
	nvme_op_rsrc_pool->n_free--;
	WARN_ON(nvme_op_rsrc_pool->n_free < 0);
	spin_unlock_irqrestore(&nvme_op_rsrc_pool->lock, flags);

out:
	return rsrc;
}

static void return_nvme_op_rsrc_sync(struct nvmeibs_serjio_disk_private_data *serjio_pd,
									 struct nvme_op_rsrc *op_rsrc, enum nvme_op_state exp_op_state)
{
	struct nvme_op_rsrc_pool *nvme_op_rsrc_pool = &serjio_pd->nvme_op_rsrc_pool;
	unsigned long flags;

	nvme_op_rsrc_chng_state(op_rsrc, exp_op_state, NVME_OP_FREE);
	spin_lock_irqsave(&nvme_op_rsrc_pool->lock, flags);
	INIT_LIST_HEAD(&op_rsrc->link);
	op_rsrc->range_idx = -1;
	op_rsrc->binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
	op_rsrc->entry = -1;
	op_rsrc->status = 0;
	op_rsrc->comp = NULL;
	op_rsrc->comp_ctr = NULL;
	op_rsrc->param = NULL;
	reset_nvme_op_rsrc_len_md(op_rsrc);
	list_add_tail(&op_rsrc->link, &nvme_op_rsrc_pool->free_list);
	nvme_op_rsrc_pool->n_free++;
	if (nvme_op_rsrc_pool->n_waiters > 0)
		complete(&nvme_op_rsrc_pool->free_comp);
	spin_unlock_irqrestore(&nvme_op_rsrc_pool->lock, flags);
	WARN_ON_ONCE(nvme_op_rsrc_pool->n_free > NUM_OF_NVME_OP_RSRC);
}

static int submit_nvme_op_rsrc_to_disk(struct nvme_op_rsrc *op_rsrc, enum nvme_op_state exp_op_state)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int rv;
	int nvme_op = op_rsrc->nvme_req.nvme_op;
	u64 submitted_bytes = op_rsrc->nvme_req.data_len;
	u64 block_size = op_rsrc->nvme_req.use_hw_blocks ? (1 << nvmeibs_disk_info_get_block_shift(serjio_pd->di)) : NVMEIBC_SECTOR_SIZE;

	BUG_ON(op_rsrc->nvme_req.data_len > (unsigned)(op_rsrc->n_data_pgs << PAGE_SHIFT));

	nvme_op_rsrc_chng_state(op_rsrc, exp_op_state, NVME_OP_POSTED);
	op_rsrc->op_rsrc_stats.t_start = ktime_get();
	if ((rv = nvmeibs_serjio_update_disk(serjio_pd->di, &op_rsrc->nvme_req)) < 0) {
		nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_ERROR);
		nvmeibs_serjio_work_type_submit_io_fail(&serjio_pd->stats, serjio_pd->current_work_type, nvme_op);
	} else {

		nvmeibs_serjio_work_type_submit_io_success(&serjio_pd->stats, serjio_pd->current_work_type, nvme_op, submitted_bytes, block_size);
	}
	
	return rv;
}

static void write_range_db_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[op_rsrc->range_idx];
	unsigned long flags;

	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (op_rsrc->status) {
		struct jranges_allocation_table *jranges_alloc_tbl = &jrange_entry->serjio_pd->jranges_alloc_tbl;
		_NEs(error_serjio_write_range_db_cb, serjio_pd,
				"Failed (@STATUS) to write to DB for Range @JRNL_RNG_IDX (LBA @DISK_BLOCK)",
				status, jrange_entry->range_idx, (long unsigned)op_rsrc->nvme_req.disk_block);
		spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
		switch(jrange_entry->status) {
		case JRANGE_FREE:
		case JRANGE_QUARANTINED:
			hlist_del(&jrange_entry->link);
			if (jrange_entry->status == JRANGE_FREE)
				jranges_alloc_tbl->num_free_rngs--;
			else
				jranges_alloc_tbl->num_quarantined_rngs--;
			jrange_entry->status = JRANGE_DB_ERR;
			hlist_add_head(&jrange_entry->link, &jranges_alloc_tbl->db_err_ranges);
			jranges_alloc_tbl->num_db_err_rngs++;
			break;
		case JRANGE_ALLOCATED:
		case JRANGE_RESERVED:
			jrange_entry->status = JRANGE_RSVD_DB_ERR;
			break;
		default:
			BUG();
		}
		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
	} else {
		_NTs(trace_serjio_write_range_db_cb, serjio_pd, "Wrote Client @CLIENT_UUID (@CLIENT_ID) to DB for Range @JRNL_RNG_IDX (LBA @DISK_BLOCK)",
		    &jrange_entry->client_uuid, jrange_entry->client_id,
			op_rsrc->range_idx, (long unsigned)op_rsrc->nvme_req.disk_block);
		jrange_entry->ent_gen_id_wrapped = false;
	}

	op_rsrc->status = status;
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(op_rsrc->serjio_pd, op_rsrc, NVME_OP_CB);
}

static inline u32 serjio_db_crc32(const void *buf, unsigned long len)
{
	return (crc32(~0L, buf, len) ^ ~0L);
}

static int write_jrange_to_serjio_db_from_cb(
	struct jrange_entry *jrange_entry, struct nvme_op_rsrc *op_rsrc, enum nvme_op_state exp_op_state)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrange_entry->serjio_pd;
	struct serjio_db_jrange_entry *db_entry;
	u32 range_idx = jrange_entry->range_idx;
	int rv;

	/* For debugging */
	op_rsrc->range_idx = range_idx;

	/* Fill the DB Entry */
	db_entry = (void *)op_rsrc->virt;
	memset(db_entry, 0, SERJIO_DB_ENTS_TO_BYTES(serjio_pd->di, 1));
	db_entry->hdr.magic = SERJIO_DB_JRANGE_ENTRY_MAGIC;
	db_entry->hdr.version_major = SERJIO_DB_CURR_VERSION_MAJOR;
	db_entry->hdr.version_minor = SERJIO_DB_CURR_VERSION_MINOR;
	db_entry->hdr.len_8b = cpu_to_be16(SERJIO_DB_ENTS_TO_8B(serjio_pd->di, 1));
	db_entry->data.range_idx = cpu_to_be32((u32)range_idx);
	db_entry->data.range_status = cpu_to_be32((u32)
		(jrange_entry->status == JRANGE_QUARANTINED ? JRANGE_FREE : jrange_entry->status));
	db_entry->data.binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
	db_entry->data.rng_rlba = cpu_to_be32(jrange_entry->rng_rlba);
	db_entry->data.rng_nlba = cpu_to_be32(jrange_entry->rng_nlba);
	db_entry->data.rng_rblk = cpu_to_be32(jrange_entry->rng_rblk);
	db_entry->data.rng_nblk = cpu_to_be32(jrange_entry->rng_nblk);

	if (jrange_entry->status == JRANGE_ALLOCATED || jrange_entry->status == JRANGE_RESERVED) {
		db_entry->data.last_client_id = cpu_to_be64(jrange_entry->client_id);
		db_entry->data.client_uuid = jrange_entry->client_uuid;
		strlcpy(db_entry->data.client_host, jrange_entry->client_host, sizeof(db_entry->data.client_host));
		db_entry->data.reserved_secs = cpu_to_be64((u64)jrange_entry->reserved.tv_sec);
		db_entry->data.last_allocated_secs = cpu_to_be64((u64)jrange_entry->last_allocated.tv_sec);
		db_entry->data.last_returned_secs = cpu_to_be64((u64)jrange_entry->last_returned.tv_sec);
		db_entry->data.binje_shift = jrange_entry->binje_shift;
	}

	db_entry->data.gen_id = cpu_to_be64((u64)jrange_entry->gen_id);
	db_entry->data.init_db_uuid = jrange_entry->init_db_uuid;
	db_entry->data.init_db_uuid_done = jrange_entry->init_db_uuid_done;

	db_entry->hdr.crc32 = cpu_to_be32(serjio_db_crc32(
		db_entry, SERJIO_DB_ENTS_TO_BYTES(serjio_pd->di, 1)));

	/* Fill the NVME Request */
	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_write, false,
			SERJIO_DB_ENT_TO_NVMEIBC_SECT(serjio_pd, range_idx),
			SERJIO_DB_ENTS_TO_BYTES(serjio_pd->di, 1),
			write_range_db_cb, NULL, NULL, 0);

	_NDs(trace_serjio_write_jrange_to_serjio_db_from_cb, serjio_pd, "disk_block: @DISK_BLOCK data_len: @DATA_LEN block_shift: @BLOCK_SHIFT_INT",
			(long unsigned)op_rsrc->nvme_req.disk_block, op_rsrc->nvme_req.data_len, nvmeibs_disk_info_get_block_shift(serjio_pd->di));

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, exp_op_state))) {
		_NEs(error_serjio_write_jrange_to_serjio_db_from_cb, serjio_pd, "Failed (@RV) to execute local nvme command", rv);
	}

	return rv;
}

static int write_jrange_to_serjio_db(struct jrange_entry *jrange_entry,
									 struct completion *comp, atomic_t *comp_ctr)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrange_entry->serjio_pd;
	int rv = 0;
	struct nvme_op_rsrc *op_rsrc;
	DECLARE_COMPLETION_ONSTACK(int_comp);
	int ctr_val;

	NFIN;
	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd,
		NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		_NEs(error_serjio_write_jrange_to_serjio_db, serjio_pd, "Could not get nvme op rsrc");
		rv = -ENOMEM;
		goto out;
	}

	if (comp) {
		op_rsrc->comp = comp;
		if (comp_ctr) {
			op_rsrc->comp_ctr = comp_ctr;
			ctr_val = atomic_inc_return(comp_ctr);
			_NDs(trace_serjio_write_jrange_to_serjio_db_ctr_inc, serjio_pd,
			     "comp_ptr: @PTR, value: @COUNT", comp_ctr, ctr_val);
		}
	} else
		op_rsrc->comp = &int_comp;

	if ((rv = write_jrange_to_serjio_db_from_cb(jrange_entry, op_rsrc, NVME_OP_FREE))) {
		if (comp_ctr) {
			ctr_val = atomic_dec_return(comp_ctr);
			_NDs(trace_serjio_write_jrange_to_serjio_db_ctr_dec, serjio_pd,
			     "comp_ptr: @PTR, value: @COUNT", comp_ctr, ctr_val);
		}
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
	} else if (!comp) {
		/* Wait for request to complete */
		wait_for_completion(&int_comp);
	}

out:
	NFOUT;
	return rv;
}

static void zero_journal_entry_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int range_idx = op_rsrc->range_idx;
	int binje_shift = op_rsrc->binje_shift;
	int entry_idx = op_rsrc->entry;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	enum nvmeibs_serjio_jentry_state prev_jentry_state;
	union jblock_md *jmdc_entry = get_jmdc_entry(jrange_entry, entry_idx);
	enum nvmeibs_serjio_jentry_state zero_ok_state = (enum nvmeibs_serjio_jentry_state)op_rsrc->param;

	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status) {
		/* TBD: Should we have a special state for IO failure */
		prev_jentry_state = JENTRY_STATE_CHNG(
			jrange_entry, entry_idx, JENTRY_IO_ERR, false, JENTRY_STATE_CHNG_REASON_ZERO_FAIL);
		nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << binje_shift);
		_NTs(error_serjio_zero_journal_entry_cb, serjio_pd, "Error (@STATUS) zeroing journal entry (@JRNL_RNG_IDX,@BINJE,@JRNL_RNG_ENT_IDX) - state @JENTRY_STATE -> @JENTRY_STATE",
			status, range_idx, binje_shift, entry_idx,
			prev_jentry_state, JENTRY_UNKNOWN);
	} else {
		prev_jentry_state = JENTRY_STATE_CHNG(
			jrange_entry, entry_idx, zero_ok_state, false, JENTRY_STATE_CHNG_REASON_ZERO_OK);
		nvmeib_shared_set_jentry_md_unused(jmdc_entry, 1 << binje_shift);
		_NDs(trace_serjio_zero_journal_entry_cb, serjio_pd, "Journal Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX @JENTRY_STATE => @JENTRY_STATE", range_idx, binje_shift, entry_idx, prev_jentry_state, zero_ok_state);
	}
	op_rsrc->status = status;
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr) {
			complete(op_rsrc->comp);
		} else {
			int ctr_val = atomic_dec_return(op_rsrc->comp_ctr);
			_NDs(trace_serjio_zero_journal_entry_cb_ctr, serjio_pd, "dec comp_ctr: @PTR value: @COUNT", op_rsrc->comp_ctr, ctr_val);
			if (!ctr_val)
				complete(op_rsrc->comp);
		}
	}
	return_nvme_op_rsrc_sync(op_rsrc->serjio_pd, op_rsrc, NVME_OP_CB);
}

static inline int zero_journal_entry_from_cb_vargs(struct nvme_op_rsrc *op_rsrc, nvme_callback_t *cb, void *cb_param,
												   enum nvme_op_state exp_op_state,
												   const char *data_str_fmt, va_list data_str_args)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	union jblock_md jmd_unused[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];
	size_t md_sz;
	int rv = 0;

	/* Set journal data and meta-data */
	memset(op_rsrc->virt, 0, op_rsrc->n_pages << PAGE_SHIFT);
	vsnprintf((char *)op_rsrc->virt, PAGE_SIZE, data_str_fmt, data_str_args);

	/* Set meta-data to jmdc unused value */
	md_sz = nvmeib_shared_set_jentry_md_unused(jmd_unused, 1 << op_rsrc->binje_shift);

	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_write, false,
		JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, op_rsrc->range_idx, op_rsrc->entry),
		JOURNAL_ENTS_TO_BYTES(serjio_pd->di, op_rsrc->binje_shift, 1),
		(cb ? cb : zero_journal_entry_cb),
		(cb_param ? cb_param : (void *)JENTRY_FREE),
		jmd_unused, md_sz);

	_NDs(trace_serjio_zero_journal_entry_from_cb, serjio_pd, "Zeroing Journal Entry @JRNL_RNG_ENT_IDX of Range @JRNL_RNG_IDX", op_rsrc->entry,
		op_rsrc->range_idx);

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, exp_op_state))) {
		_NEs(error_serjio_zero_journal_entry_from_cb, serjio_pd,
				"Failed (@RV) to execute local nvme command. op_rsrc @PTR",
				rv, op_rsrc);
	}

	return rv;
}

static /*inline*/ int zero_journal_entry_from_cb(struct nvme_op_rsrc *op_rsrc, nvme_callback_t *cb, void *cb_param,	// Error: can never be inlined because it uses variable argument lists
											 enum nvme_op_state exp_op_state, const char *data_str_fmt, ...)
{
	va_list data_str_args;
	int rv;

	va_start(data_str_args, data_str_fmt);
	rv = zero_journal_entry_from_cb_vargs(op_rsrc, cb, cb_param, exp_op_state, data_str_fmt, data_str_args);
	va_end(data_str_args);

	return rv;
}

static int zero_journal_entry(struct nvmeibs_serjio_disk_private_data *serjio_pd,
							  struct jrange_entry *jrange_entry, unsigned entry, atomic_t *comp_ctr,
							  struct completion *comp, nvme_callback_t *cb, void *cb_param,
							  const char *data_str_fmt, ...)
{
	struct nvme_op_rsrc *op_rsrc;
	int rv = 0;
	va_list data_str_args;
	int ctr_val;

	NFIN;
	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd,
		NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		_NEs(error_serjio_zero_journal_entry, serjio_pd, "Could not get nvme op rsrc");
		rv = -ENOMEM;
		goto out;
	}
	if (entry < 0 || entry >= jrange_entry->n_ents) {
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_FREE);
		_NEs(error_1_serjio_zero_journal_entry, serjio_pd, "Invalid Journal Entry @JRNL_RNG_ENT_IDX", entry);
		rv = -EINVAL;
		goto out;
	}

	op_rsrc->range_idx = jrange_entry->range_idx;
	op_rsrc->binje_shift = jrange_entry->binje_shift;
	op_rsrc->entry = entry;
	op_rsrc->comp = comp;
	if (comp_ctr) {
		ctr_val = atomic_inc_return(comp_ctr);
		_NDs(trace_serjio_zero_journal_entry_inc_ctr_val, serjio_pd, "inc comp_ctr: @PTR value: @COUNT", comp_ctr, ctr_val);
		op_rsrc->comp_ctr = comp_ctr;
	}

	va_start(data_str_args, data_str_fmt);
	if ((rv = zero_journal_entry_from_cb_vargs(op_rsrc, cb, cb_param, NVME_OP_FREE, data_str_fmt, data_str_args)) < 0) {
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
		ctr_val = atomic_dec_return(comp_ctr);
		_NDs(trace_serjio_zero_journal_entry_dec_ctr_val, serjio_pd, "dec comp_ctr: @PTR value: @COUNT", comp_ctr, ctr_val);
	}
	va_end(data_str_args);

out:
	NFOUT;
	return rv;
}

static void zero_journal_range_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int range_idx = op_rsrc->range_idx;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	enum nvmeibs_serjio_jentry_state prev_jentry_state;
	unsigned i;

	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status) {
		_NTs(zero_journal_range_cb_t1, serjio_pd, "Error (@INT) zeroing journal range (@INT)", status, range_idx);
		for (i = 0; i < jrange_entry->n_ents; i++) {
			union jblock_md *jmdc_entry = get_jmdc_entry(jrange_entry, i);
			nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << jrange_entry->binje_shift);
			prev_jentry_state = JENTRY_STATE_CHNG(
				jrange_entry, i, JENTRY_IO_ERR, false, JENTRY_STATE_CHNG_REASON_ZERO_FAIL);
		}
	} else {
		for (i = 0; i < jrange_entry->n_ents; i++) {
			union jblock_md *jmdc_entry = get_jmdc_entry(jrange_entry, i);
			nvmeib_shared_set_jentry_md_unused(jmdc_entry, 1 << jrange_entry->binje_shift);
			prev_jentry_state = JENTRY_STATE_CHNG(
				jrange_entry, i, JENTRY_FREE, false, JENTRY_STATE_CHNG_REASON_ZERO_OK);
			_NDs(zero_journal_range_cb_d1, serjio_pd, "Journal Range @INT N @BINJE Entry @INT @SERJIO_STATE => @SERJIO_STATE",
				range_idx, jrange_entry->binje_shift, i, prev_jentry_state, JENTRY_FREE);
		}
	}
	op_rsrc->status = status;
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(op_rsrc->serjio_pd, op_rsrc, NVME_OP_CB);
}


static int zero_journal_range(struct jrange_entry *jrng, atomic_t *comp_ctr,
							  struct completion *comp)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	struct nvme_op_rsrc *op_rsrc;
	int rv = 0;
	unsigned i;

	NFIN;
	if (nvmeib_jmd_unused_entry_val.raw == 0) {
		/* Can use write zeroes */
		if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd,
			NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
			_NEs(zero_journal_range_e1, serjio_pd, "Could not get nvme op rsrc");
			rv = -ENOMEM;
			goto out;
		}

		op_rsrc->range_idx = jrng->range_idx;
		op_rsrc->binje_shift = jrng->binje_shift;
		op_rsrc->comp = comp;

		init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_write_zeroes, false,
			JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, op_rsrc->range_idx, 0),
			JOURNAL_ENTS_TO_BYTES(serjio_pd->di, jrng->binje_shift, jrng->n_ents),
			zero_journal_range_cb, NULL, NULL, 0);

		if (comp_ctr) {
			atomic_inc(comp_ctr);
			op_rsrc->comp_ctr = comp_ctr;
		}

		_NTs(zero_journal_range_t1, serjio_pd, "Zeroing Journal Range @INT", jrng->range_idx);

		/* Submit the request */
		if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, NVME_OP_FREE))) {
			return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
			if (comp_ctr)
				atomic_dec(comp_ctr);
			_NEs(zero_journal_range_e2, serjio_pd, "Failed (@INT) to execute local nvme command", rv);
		}
		goto out;
	} else {
		/* TBD: Update PRPL to write the same data to multiple entries */
		for (i = 0; i < jrng->n_ents; i++) {
			if ((rv = zero_journal_entry(serjio_pd, jrng, i, comp_ctr, comp, NULL, NULL,
				"%s - rng: %u entry: %d", __func__, jrng->range_idx, i)))
				goto out;
		}
	}

out:
	NFOUT;
	return rv;
}

static void sync_ent_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int range_idx = op_rsrc->range_idx;
	int binje_shift = op_rsrc->binje_shift;
	int entry_idx = op_rsrc->entry;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	union jblock_md jmdc_entry[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];

	(void)result;
	NFIN;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status != 0) {
		_NEs(error_serjio_sync_ent_cb, serjio_pd, "Error (@STATUS) reading journal entry", status);
		nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << binje_shift);
		set_jmdc_entry_data(jrange_entry, entry_idx, jmdc_entry);
		JENTRY_STATE_CHNG(jrange_entry, entry_idx, JENTRY_IO_ERR, false, JENTRY_STATE_CHNG_REASON_SYNC_FAIL);
		goto return_op_rsrc;
	}

	get_nvme_op_rsrc_md(op_rsrc, jmdc_entry, sizeof(jmdc_entry[0]) << binje_shift);

	_NDs(trace_serjio_sync_ent_cb, serjio_pd, "Read Journal Metadata @JMDC_ENT for Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX (Disk LBA: @DISK_BLOCK)",
	    *(u64*)&jmdc_entry, range_idx, 1 << binje_shift, entry_idx, (long unsigned)op_rsrc->nvme_req.disk_block);

	/* Copy metadata read from the journal to RAM */
	if (set_jmdc_entry_data(jrange_entry, entry_idx, jmdc_entry)) {
		_NEs(error_1_serjio_sync_ent_cb, serjio_pd, "Failed to set jmdc entry");
		goto return_op_rsrc;
	}

	/* Set state from unknown to dirty or free */
	if (nvmeib_is_jmd_unused_entry(jmdc_entry))
		JENTRY_STATE_CHNG(jrange_entry, entry_idx, JENTRY_FREE, false, JENTRY_STATE_CHNG_REASON_SYNC_FREE);
	else {
		u64 j2d_start = 0, j2d_end = 0;
		int chain_err = 0;
		SERJIO_BUG_ON(!nvmeib_is_jmd_io_entry(jmdc_entry) ||
				(chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << binje_shift, &j2d_start, &j2d_end)) != NVMEIB_JENTRY_CHAIN_OK,
				error_sync_ent_cb_jmdc_unexpect, serjio_pd,
						"Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX N: @BINJE - Invalid JMDC Value @JMDC_ENT, Chain Error @ERROR_STR, Chain Error Block @IDX",
						range_idx, entry_idx, 1 << binje_shift, jmdc_entry[0].raw,
						nvmeib_shared_jentry_md_chain_err_str(chain_err), nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
		JENTRY_STATE_CHNG(jrange_entry, entry_idx, JENTRY_SYNCED, false,
				  JENTRY_STATE_CHNG_REASON_SYNC_DIRTY);
		if (!is_j2d_in_valid_segment(serjio_pd, j2d_start, j2d_end, NULL, NULL)) {
			_NTs(trace_serjio_sync_ent_cb_jmdc_inval, serjio_pd,
				 "Zeroing Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX with invalid J2D: [@J2D_START, @J2D_END]",
				range_idx, entry_idx, j2d_start, j2d_end);
			if (zero_journal_entry_from_cb(op_rsrc, NULL, NULL, NVME_OP_CB,
					"%s - rng: %u N: %d entry: %u status: %s j2d: [%u,%u] not in valid seg\n", __func__,
					range_idx, binje_shift, entry_idx, nvmeib_shared_serjio_jrange_status_to_str(jrange_entry->status), j2d_start, j2d_end))
				goto return_op_rsrc;
			else
				goto out;
		}
	}

return_op_rsrc:
	/* This will also signal the completion that the io thread is waiting for */
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
out:
	NFOUT;
}

static int chk_wait_ret_cln_disk_rng(struct jrange_entry *jrng)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	int rv = 0;
	unsigned entry;
	enum nvmeibs_serjio_jentry_state jentry_state;
	atomic_t ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(comp);
	unsigned long flags;
	struct seg_tree_entry *seg_iter, *tmp;
	int n_zero_rng = 0;
	struct seg_tree_entry *j2d_seg;

	NFIN;
	/* Sync all entries that we were waiting to return */
	down_read(&serjio_pd->gpt_rwsem);
	if ((rv = rd_jrange(serjio_pd, sync_ent_cb, NULL, jrng,
		JENTRY_WAIT_RET_MASK, NULL, NULL, NULL)) < 0) {
		up_read(&serjio_pd->gpt_rwsem);
		goto out;
	}

	/* Zero any entries that are in any of the LBA ranges we are waiting to clean */
	spin_lock_irqsave(&jrng->lock, flags);
	for (entry = 0; entry < jrng->n_ents; entry++) {
		union jblock_md *jmdc_entry = get_jmdc_entry(jrng, entry);
		u64 j2d;
		jentry_state = GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, entry);
		SERJIO_BUG_ON(JENTRY_OWNED_BY_JAM(jentry_state),
					error_chk_wait_ret_cln_disk_rng_inv_jentry_state, serjio_pd,
					"Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX Invalid Jentry State: @JENTRY_STATE",
					jrng->range_idx, entry, jentry_state);
		if (jentry_state == JENTRY_FREE)
			continue;
		j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_entry);
		if ((j2d_seg = get_j2d_cln_rng(serjio_pd, j2d, (1 << CLN_SEG_WAIT_RNG_RETURN), jrng->range_idx, false))) {
			spin_unlock_irqrestore(&jrng->lock, flags);
			/* Entry points at a clean segment => Zero it */
			n_zero_rng++;
			if ((rv = zero_journal_entry(serjio_pd, jrng, entry, &ctr, &comp, NULL, NULL,
				"%s - rng: %u binje: %u entry: %d has j2d: %llu in seg: %s\n", __func__,
				jrng->range_idx, 1 << jrng->binje_shift, entry, j2d, j2d_seg->seg_uuid_str))) {
				if (atomic_dec_return(&ctr) > 0)
					wait_for_completion(&comp);
				_NEs(error_serjio_chk_wait_ret_cln_disk_rng, serjio_pd, "zero_journal_entry failed (@RV) for range @JRNL_RNG_IDX entry @JRNL_RNG_ENT_IDX",
					rv, jrng->range_idx, entry);
				rv = -EIO;
				up_read(&serjio_pd->gpt_rwsem);
				goto out;
			}
			spin_lock_irqsave(&jrng->lock, flags);
		}
	}
	spin_unlock_irqrestore(&jrng->lock, flags);
	up_read(&serjio_pd->gpt_rwsem);

	if (atomic_dec_return(&ctr) > 0)
		wait_for_completion(&comp);

	_NTs(trace_serjio_chk_wait_cln_disk_rng_zero_ent, serjio_pd,
		"Zeroed @NUM_ENTS entries from range @JRNL_RNG_IDX due to pending cleans\n",
		n_zero_rng, jrng->range_idx);

	/* Now clear the journal range from the wait return bitmaps */
	list_for_each_entry_safe(seg_iter, tmp, &serjio_pd->cln_seg_wait_ret_list, cln_link) {
		spin_lock_irqsave(&seg_iter->cln_lock, flags);
		SERJIO_BUG_ON(seg_iter->cln_state != CLN_SEG_WAIT_RNG_RETURN,
					  error_chk_wait_ret_cln_disk_rng_inv_cln_state, serjio_pd,
						"Seg @SEG_UUID_STR (@SEG_TREE_ENTRY) in invalid clean state @CLN_SEG_STATE",
						seg_iter->seg_uuid_str, seg_iter, seg_iter->cln_state);
		if (test_and_clear_bit(jrng->range_idx, seg_iter->wait_rng_bmp)) {
			BUG_ON(seg_iter->wait_rng_cnt == 0);
			seg_iter->wait_rng_cnt--;
			if (seg_iter->wait_rng_cnt == 0) {
				/* Clean is done, notify TOMA */
				BUG_ON(!bitmap_empty(seg_iter->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
				seg_iter->cln_state = seg_iter->seg_delete ? CLN_SEG_DONE : CLN_SEG_IDLE;
				spin_unlock_irqrestore(&seg_iter->cln_lock, flags);
				list_del_init(&seg_iter->cln_link);
				if (seg_iter->seg_delete && seg_iter->gpt_idx == -1) {
					/* Segment is not in the GPT, so we can simply remove it from the interval tree and free it */
					_NTs(trace_serjio_chk_wait_cln_disk_rng_finish_cln_del_seg, serjio_pd,
						 "Finished cleaning deleted segment @SEG_UUID_STR\n", seg_iter->seg_uuid_str);
					hash_del(&seg_iter->link);
					seg_tree_remove(seg_iter, &serjio_pd->del_seg_rb_root);
					kfree(seg_iter);
				} else {
					_NTs(trace_serjio_chk_wait_cln_disk_rng_finish_cln_seg, serjio_pd,
						 "Finished cleaning segment @SEG_UUID_STR. Reporting to TOMA\n", seg_iter->seg_uuid_str);
					if ((rv = nvmeibs_toma_report_event_serjio_disk_range_cleaned(
						serjio_pd->di, seg_iter->seg_uuid_str))) {
						_NEs(error_serjio_chk_wait_cln_disk_rng_report_toma, serjio_pd,
							 "Failed (@RV) to report clean segment @SEG_UUID_STR to TOMA\n",
							rv, seg_iter->seg_uuid_str);
					}
				}
			}
		} else
			spin_unlock_irqrestore(&seg_iter->cln_lock, flags);
	}
	if (list_empty(&serjio_pd->cln_seg_wait_ret_list)) {
		_NTs(trace_serjio_chk_wait_cln_disk_rng_go_ready, serjio_pd,
			 "cln_seg_wait_ret_list empty, returning to Ready State\n");
		BUG_ON(serjio_state_cmp_exch(
			serjio_pd, SERJIO_CLN_JRNL, SERJIO_READY) != SERJIO_CLN_JRNL);
	} else {
		list_for_each_entry(seg_iter, &serjio_pd->cln_seg_wait_ret_list, cln_link) {
			spin_lock_irqsave(&seg_iter->cln_lock, flags);
			_NTs(trace_serjio_chk_wait_cln_disk_rng_still_waiting, serjio_pd,
				 "Segment @SEG_UUID_STR - still waiting on return ranges: " NVMEIB_EC_JOURNAL_RANGES_TRACE "\n",
				seg_iter->seg_uuid_str, seg_iter->wait_rng_bmp);
			spin_unlock_irqrestore(&seg_iter->cln_lock, flags);
		}
	}

out:
	NFOUT;
	return rv;
}

#define CMP_N_BYTES 8

static bool is_nvme_block_zeroed(struct nvme_op_rsrc *op_rsrc, bool check_md)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	struct nvmeibs_disk_info *di = serjio_pd->di;
	size_t md_sz;
	bool is_zeroed = true;
	u8 data_zero_val[CMP_N_BYTES];
	u8 md_zero_val[CMP_N_BYTES];
	u32 *cmp_ptr, *end_ptr;

	memset(data_zero_val, DISK_DATA_INIT_BYTE, CMP_N_BYTES);
	memset(md_zero_val, DISK_MD_INIT_BYTE, CMP_N_BYTES);

	/* data */
	BUG_ON(op_rsrc->nvme_req.data_len % CMP_N_BYTES != 0);
	end_ptr = op_rsrc->virt + op_rsrc->nvme_req.data_len;
	for (cmp_ptr = op_rsrc->virt; cmp_ptr < end_ptr; cmp_ptr += CMP_N_BYTES) {
		if (memcmp(cmp_ptr, data_zero_val, CMP_N_BYTES) != 0) {
			is_zeroed = false;
			goto out;
		}
	}

	if (!check_md)
		goto out;

	/* md */
	md_sz = NVMEIB_D2MD_LEN(op_rsrc->nvme_req.data_len, nvmeibs_disk_info_get_block_shift(di), nvmeibs_disk_info_get_md_size(di));
	BUG_ON(md_sz % CMP_N_BYTES != 0);
	cmp_ptr = nvmeibs_disk_info_has_mtdt_extd(di) ?
		op_rsrc->virt + op_rsrc->nvme_req.data_len :
		op_rsrc->virt + (op_rsrc->n_data_pgs << PAGE_SHIFT);
	end_ptr = (void *)cmp_ptr + md_sz;
	for (; cmp_ptr < end_ptr; cmp_ptr += CMP_N_BYTES) {
		if (memcmp(cmp_ptr, md_zero_val, CMP_N_BYTES) != 0) {
			is_zeroed = false;
			goto out;
		}
	}

out:
	return is_zeroed;
}

#undef CMP_N_BYTES

static void read_serjio_db_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *jrange;
	unsigned range_idx = op_rsrc->range_idx;
	struct serjio_db_jrange_entry *db_entry = op_rsrc->virt;
	unsigned long flags;
	u32 orig_crc32 = be32_to_cpu(db_entry->hdr.crc32);
	u16 db_entry_len_8b = be16_to_cpu(db_entry->hdr.len_8b);
	u16 db_entry_ver_code = SERJIO_DB_VERSION_CODE(db_entry->hdr.version_major, db_entry->hdr.version_minor);
	/* VERSION 1.1 fields */
	u32 db_entry_idx = be32_to_cpu(db_entry->data.range_idx);
	u32 db_entry_status = be32_to_cpu(db_entry->data.range_status);
	/* Version 1.2 fields */
	u8 db_ent_binje_shift = db_entry_ver_code >= SERJIO_DB_VERSION_1_2_CODE ? db_entry->data.binje_shift : ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
	u32 db_ent_rng_rlba = db_entry_ver_code >= SERJIO_DB_VERSION_1_3_CODE ?
		be32_to_cpu(db_entry->data.rng_rlba) : JOURNAL_BLKS_TO_DISK_LBAS(serjio_pd->di, range_idx << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT);
	u32 db_ent_rng_nlba = db_entry_ver_code >= SERJIO_DB_VERSION_1_3_CODE ?
		be32_to_cpu(db_entry->data.rng_nlba) : JOURNAL_BLKS_TO_DISK_LBAS(serjio_pd->di, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3);
	u32 db_ent_rng_rblk = db_entry_ver_code >= SERJIO_DB_VERSION_1_3_CODE ?
		be32_to_cpu(db_entry->data.rng_rblk) : (range_idx << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT);
	u32 db_ent_rng_nblk = db_entry_ver_code >= SERJIO_DB_VERSION_1_3_CODE ?
		be32_to_cpu(db_entry->data.rng_nblk) : (1 << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT);
	uuid_be init_db_uuid = db_entry_ver_code >= SERJIO_DB_VERSION_1_4_CODE ? db_entry->data.init_db_uuid : NULL_UUID_BE;
	uuid_be init_db_uuid_done = db_entry_ver_code >= SERJIO_DB_VERSION_1_4_CODE ? db_entry->data.init_db_uuid_done : NULL_UUID_BE;

	(void)result;

	NFIN;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (IS_SERJIO_DYING(serjio_pd)) {
		_NTs(trace_serjio_read_serjio_db_cb, serjio_pd, "Serjio for disk @DISK_ID_STR is going down", nvmeibs_disk_info_get_disk_id(serjio_pd->di));
		goto return_op_rsrc;
	}

	if (status != 0) {
		_NEs(error_serjio_read_serjio_db_cb, serjio_pd, "Error (@STATUS) reading serjio DB", status);
		db_entry_status = JRANGE_DB_ERR;
	} else {
		db_entry->hdr.crc32 = 0;
		/* TBD: Use VEX for this */
		if (db_entry->hdr.magic != SERJIO_DB_JRANGE_ENTRY_MAGIC ||
			db_entry_ver_code < SERJIO_DB_MIN_VERSION_CODE ||
			db_entry_ver_code > SERJIO_DB_MAX_VERSION_CODE ||
			/* VERSION 1.1 fields */
			db_entry_len_8b != SERJIO_DB_ENTS_TO_8B(serjio_pd->di, 1) ||
			db_entry_idx != (u32)range_idx ||
			db_entry_status < JRANGE_STATUS_DB_MIN ||
			(db_entry_status > JRANGE_STATUS_DB_V12_MAX && db_entry_ver_code < SERJIO_DB_VERSION_1_3_CODE) ||
			(db_entry_status > JRANGE_STATUS_DB_MAX) ||
			(db_entry_status != JRANGE_FREE && db_entry_status != JRANGE_INVALID &&
			nvmeib_uuid_cmp(db_entry->data.client_uuid, NULL_UUID_BE) == 0) ||
			(db_entry_status == JRANGE_FREE &&
			nvmeib_uuid_cmp(db_entry->data.client_uuid, NULL_UUID_BE) != 0) ||
			(db_entry_status == JRANGE_INVALID &&
			nvmeib_uuid_cmp(db_entry->data.client_uuid, NULL_UUID_BE) != 0) ||
			/* Version 1.2 fields */
			db_ent_binje_shift < ilog2(NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY) || db_ent_binje_shift > ilog2(NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY) ||
			/* Version 1.3 fields */
			(db_ent_rng_nlba != 0 && db_entry_status == JRANGE_INVALID) ||
			(db_ent_rng_nlba == 0 && db_entry_status != JRANGE_INVALID) ||
			(db_ent_rng_nlba > JOURNAL_BLKS_TO_DISK_LBAS(serjio_pd->di, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE)) ||
			(db_ent_rng_rlba != NVMEIB_EC_INVALID_JOURNAL_RLBA && db_entry_status == JRANGE_INVALID) ||
			(db_ent_rng_rlba >= JOURNAL_BLKS_TO_DISK_LBAS(serjio_pd->di, (unsigned)(NVMEIB_EC_MAX_JOURNAL_RANGES << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT)) && db_entry_status != JRANGE_INVALID) ||
			(db_ent_rng_nblk != 0 && db_entry_status == JRANGE_INVALID) ||
			(db_ent_rng_nblk == 0 && db_entry_status != JRANGE_INVALID) ||
			(db_ent_rng_nblk > NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE) ||
			(db_ent_rng_rblk != NVMEIB_EC_INVALID_JOURNAL_RBLK && db_entry_status == JRANGE_INVALID) ||
			(db_ent_rng_rblk >= DISK_LBAS_TO_JOURNAL_BLKS(serjio_pd->di, serjio_pd->disk_ranges.journal.len_nlbas) && db_entry_status != JRANGE_INVALID) ||
			serjio_db_crc32(db_entry, sizeof(*db_entry)) != orig_crc32)
		{

			/* Entry is invalid - check to see if whole block is zero (could be after format) */
			if (is_nvme_block_zeroed(op_rsrc, false)) {
				db_entry_status = JRANGE_DB_ZERO;
				_NTs(trace_1_serjio_read_serjio_db_cb, serjio_pd, "Zero Serjio DB Entry @JRNL_RNG_IDX", range_idx);
			}
			else {
				db_entry_status = JRANGE_DB_ERR;
				_NWs(warn_serjio_read_serjio_db_cb, serjio_pd, "Invalid Serjio DB Entry @JRNL_RNG_IDX", range_idx);
			}
		}
	}

	spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
	jrange = &jranges_alloc_tbl->ranges[range_idx];

	/* Journal-range is in invalid list at this point */
	BUG_ON(jrange->status != JRANGE_INVALID);
	hlist_del(&jrange->link);

	jrange->status = db_entry_status;
	jrange->init_db_uuid = init_db_uuid;
	jrange->init_db_uuid_done = init_db_uuid_done;

	switch (db_entry_status) {
	case JRANGE_FREE:
		_NTs(trace_2_serjio_read_serjio_db_cb, serjio_pd, "Journal Range @JRNL_RNG_IDX free", jrange->range_idx);
		if (range_idx < NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) {
			/* [NVMESH-5279]: If the DB was init before the fix, ranges 0, 1, 2 will have been stored as JRANGE_FREE.
			 * 	==> Put in invalid list */
			_NTs(trace_serjio_read_serjio_db_cb_inv_jri, serjio_pd,
			     "Range @JRNL_RNG_IDX is invalid for client "
			     "(< NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES @JRNL_RNG_IDX). Adding to invalid list",
			     range_idx, NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES);
			jrange->status = JRANGE_QUARANTINED;
			hlist_add_head(&jrange->link, &jranges_alloc_tbl->quarantined_ranges);
			jranges_alloc_tbl->num_quarantined_rngs++;
			jranges_alloc_tbl->num_valid_rngs++;
		} else {
			/* Add to free list */
			hlist_add_head(&jrange->link, &jranges_alloc_tbl->free_ranges);
			jranges_alloc_tbl->num_free_rngs++;
			jranges_alloc_tbl->num_valid_rngs++;
		}
		jrange->gen_id = be64_to_cpu(db_entry->data.gen_id);
		jrange->binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
		jrange->rng_rlba = db_ent_rng_rlba;
		jrange->rng_nlba = db_ent_rng_nlba;
		jrange->rng_rblk = db_ent_rng_rblk;
		jrange->rng_nblk = db_ent_rng_nblk;
		/* updated by set_rng_binje */
		jrange->n_ents = 0;
		set_rng_binje(jrange, ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY), JENTRY_INVALID, JENTRY_FREE, NULL);
		break;
	case JRANGE_INVALID:
		_NTs(trace_4_serjio_read_serjio_db_cb, serjio_pd, "Journal Range @JRNL_RNG_IDX invalid", jrange->range_idx);
		/* Add to invalid ranges list */
		hlist_add_head(&jrange->link, &jranges_alloc_tbl->invalid_ranges);
		jrange->gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
		jrange->binje_shift = JRANGE_INVALID_BINJE_SHIFT;
		jrange->rng_rlba = NVMEIB_EC_INVALID_JOURNAL_RLBA;
		jrange->rng_nlba = 0;
		jrange->rng_rblk = NVMEIB_EC_INVALID_JOURNAL_RBLK;
		jrange->rng_nblk = 0;
		jrange->n_ents = 0;
		break;
	case JRANGE_DB_ZERO:
	case JRANGE_DB_ERR:
		_NTs(error_1_serjio_read_serjio_db_cb, serjio_pd, "Journal Range @JRNL_RNG_IDX DB Entry Zeroed/In-Error", jrange->range_idx);
		/* Add to err list */
		hlist_add_head(&jrange->link, &jranges_alloc_tbl->db_err_ranges);
		jranges_alloc_tbl->num_db_err_rngs++;
		if (db_entry_status == JRANGE_DB_ZERO)
			jranges_alloc_tbl->num_db_zero_rngs++;
		break;
	case JRANGE_RESERVED:
	case JRANGE_ALLOCATED:
		_NTs(trace_3_serjio_read_serjio_db_cb, serjio_pd, "Range @JRNL_RNG_IDX N @BINJE reserved for Client @CLIENT_UUID",
			jrange->range_idx, 1 << db_ent_binje_shift, &db_entry->data.client_uuid);
		/* Add to reserved list */
		strlcpy(jrange->client_host, db_entry->data.client_host, sizeof(jrange->client_host));
		jrange->client_uuid = db_entry->data.client_uuid;
		jrange->reserved.tv_sec = be64_to_cpu(db_entry->data.reserved_secs);
		jrange->reserved.tv_nsec = 0;
		jrange->last_allocated.tv_sec = be64_to_cpu(db_entry->data.last_allocated_secs);
		jrange->last_allocated.tv_nsec = 0;
		jrange->last_returned.tv_sec = be64_to_cpu(db_entry->data.last_returned_secs);
		jrange->last_returned.tv_nsec = 0;
		jrange->gen_id = be64_to_cpu(db_entry->data.gen_id);
		jrange->binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY); /* updated by set_rng_binje */
		jrange->returned_jif = jiffies;
		jrange->rng_rlba = db_ent_rng_rlba;
		jrange->rng_nlba = db_ent_rng_nlba;
		jrange->rng_rblk = db_ent_rng_rblk;
		jrange->rng_nblk = db_ent_rng_nblk;
		/* updated by set_rng_binje */
		jrange->n_ents = 0;
		set_rng_binje(jrange, db_ent_binje_shift, JENTRY_INVALID, JENTRY_UNKNOWN, NULL);
		hash_add(jranges_alloc_tbl->reserved_ranges, &jrange->link, hash_uuid(jrange->client_uuid));
		jranges_alloc_tbl->num_reserved_ranges++;
		jranges_alloc_tbl->num_valid_rngs++;
		break;
	default:
		_NEs(error_2_serjio_read_serjio_db_cb, serjio_pd, "Invalid State @DB_ENTRY_STATUS for Journal Range @JRNL_RNG_IDX",
			db_entry_status, jrange->range_idx);
		BUG_ON(1);
	}
	spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);

return_op_rsrc:
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	/* This will also signal the completion that the io thread is waiting for */
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
	NFOUT;
}

static int read_serjio_db_jrange_entry(struct nvmeibs_serjio_disk_private_data *serjio_pd,
									   int range_idx, atomic_t *read_ctr, struct completion *read_comp)
{
	int rv = 0;
	struct nvme_op_rsrc *op_rsrc;

	NFIN;
	if (range_idx < 0 || range_idx >= (int)serjio_pd->jranges_alloc_tbl.num_ranges) {
		_NEs(error_serjio_read_serjio_db_jrange_entry, serjio_pd, "Invalid journal range @JRNL_RNG_IDX", range_idx);
		rv = -EINVAL;
		goto out;
	}

	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd,
		NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		rv = -ENOMEM;
		goto out;
	}

	op_rsrc->range_idx = range_idx;
	op_rsrc->comp_ctr = read_ctr;
	op_rsrc->comp = read_comp;

	/* Fill the NVME Request */
	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_read, false,
		SERJIO_DB_ENT_TO_NVMEIBC_SECT(serjio_pd, range_idx),
		SERJIO_DB_ENTS_TO_BYTES(serjio_pd->di, 1),
		read_serjio_db_cb, NULL, NULL, 0);

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, NVME_OP_FREE))) {
		_NEs(error_1_serjio_read_serjio_db_jrange_entry, serjio_pd, "Failed (@RV) to execute local nvme command", rv);
		if (op_rsrc->comp) {
			if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
					complete(op_rsrc->comp);
		}
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
	}

	_NDs(trace_serjio_read_serjio_db_jrange_entry, serjio_pd, "Reading DB for Range @JRNL_RNG_IDX", range_idx);

out:
	NFOUT;
	return rv;
}

static inline u32 efi_crc32(const void *buf, unsigned long len)
{
	return (crc32(~0L, buf, len) ^ ~0L);
}

static void read_gpt_hdr_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	struct gpt_header *rd_gpt_hdr = op_rsrc->virt;
	struct gpt_header *out_gpt_hdr = op_rsrc->param;

	(void)result;

	NFIN;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status != 0) {
		_NEs(error_serjio_read_gpt_hdr_cb, serjio_pd, "Error (@STATUS) reading GPT Header from LBA: @DISK_BLOCK_LLONG",
			status, (u64)op_rsrc->nvme_req.disk_block);
		goto return_op_rsrc;
	}

	*out_gpt_hdr = *rd_gpt_hdr;

return_op_rsrc:
	if (op_rsrc->comp)
		complete(op_rsrc->comp);

	/* This will also signal the completion that the io thread is waiting for */
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
	NFOUT;
}

static int read_gpt_hdr(struct nvmeibs_serjio_disk_private_data *serjio_pd,
						bool primary, struct completion *read_comp, struct gpt_header *gpt_hdr)
{
	int rv = 0;
	struct nvme_op_rsrc *op_rsrc;
	sector_t disk_block;

	NFIN;

	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd, NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		rv = -ENOMEM;
		goto out;
	}

	op_rsrc->comp = read_comp;

	/* Fill the NVME Request */
	if (primary) {
		disk_block = PRIMARY_GPT_HEADER_LBA;
		_NDs(trace_serjio_read_gpt_hdr, serjio_pd, "Reading Primary GPT Header from LBA: @DISK_BLOCK_LLONG", (u64)disk_block);
	}
	else {
		disk_block = nvmeibs_disk_info_get_num_blks(serjio_pd->di, true) - 1;
		_NDs(trace_1_serjio_read_gpt_hdr, serjio_pd, "Reading Secondary GPT Header from LBA: @DISK_BLOCK_LLONG", (u64)disk_block);
	}

	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_read, true /* use_hw_blocks */,
			disk_block, DISK_LBAS_TO_BYTES(serjio_pd->di, 1),
			read_gpt_hdr_cb, gpt_hdr, NULL, 0);

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, NVME_OP_FREE))) {
		_NEs(error_serjio_read_gpt_hdr, serjio_pd, "Failed (@RV) to execute local nvme command", rv);
		if (op_rsrc->comp)
			complete(op_rsrc->comp);
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
	}

out:
	NFOUT;
	return rv;
}

struct read_gpt_ents_cb_param
{
	bool primary;
	struct gpt_header *gpt_hdr;
	struct gpt_entry *gpt_ents;
	unsigned n_gpt_ents;
};

static void read_gpt_ents_block_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	void *src_gpt_ents_block = op_rsrc->virt;
	struct read_gpt_ents_cb_param *cb_param = op_rsrc->param;
	unsigned gpt_ents_per_lba;
	unsigned gpt_ents_this_lba;

	NFIN;
	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status != 0) {
		_NEs(error_serjio_read_gpt_ents_block_cb, serjio_pd, "Error (@STATUS) reading GPT Entries Block @JRNL_RNG_IDX from LBA: @DISK_BLOCK_LLONG",
			status, op_rsrc->range_idx, (u64)op_rsrc->nvme_req.disk_block);
		goto return_op_rsrc;
	}
	gpt_ents_per_lba = DISK_LBAS_TO_BYTES(serjio_pd->di, 1) /
		le32_to_cpu(cb_param->gpt_hdr->sizeof_partition_entry);
		gpt_ents_this_lba = min(gpt_ents_per_lba, cb_param->n_gpt_ents - op_rsrc->range_idx * gpt_ents_per_lba);
	memcpy(cb_param->gpt_ents + op_rsrc->range_idx * gpt_ents_per_lba,
		   src_gpt_ents_block, gpt_ents_this_lba * sizeof(*cb_param->gpt_ents));

return_op_rsrc:
	if (op_rsrc->comp_ctr && op_rsrc->comp) {
		if (atomic_dec_return(op_rsrc->comp_ctr) == 0)
			complete(op_rsrc->comp);
	}

	/* This will also signal the completion that the io thread is waiting for */
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
	kfree(cb_param);
	NFOUT;
}

static int read_gpt_ents_block(struct nvmeibs_serjio_disk_private_data *serjio_pd,
							   bool primary, struct gpt_header *gpt_hdr,
							   struct gpt_entry *gpt_ents, unsigned n_gpt_ents, int block_idx,
							   atomic_t *read_ctr, struct completion *read_comp)
{
	int rv = 0;
	struct nvme_op_rsrc *op_rsrc;
	struct read_gpt_ents_cb_param *cb_param;

	NFIN;

	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(serjio_pd, NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		rv = -ENOMEM;
		goto out;
	}

	if (!(cb_param = kzalloc(sizeof(*cb_param), GFP_KERNEL))) {
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_FREE);
		rv = -ENOMEM;
		goto out;
	}

	cb_param->primary = primary;
	cb_param->gpt_hdr = gpt_hdr;
	cb_param->gpt_ents = gpt_ents;
	cb_param->n_gpt_ents = n_gpt_ents;

	op_rsrc->range_idx = block_idx;
	op_rsrc->comp_ctr = read_ctr;
	op_rsrc->comp = read_comp;

	/* Fill the NVME Request */
	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_read, true /* hw_blocks */,
			le64_to_cpu(gpt_hdr->partition_entry_lba) + block_idx,
			DISK_LBAS_TO_BYTES(serjio_pd->di, 1),
			read_gpt_ents_block_cb, cb_param,
			NULL, 0);

	_NTs(trace_serjio_read_gpt_ents_block, serjio_pd,
		"Reading GPT Entries Block @BLOCK_IDX (LBA: @DISK_BLOCK_LLONG)",
		block_idx, (u64)op_rsrc->nvme_req.disk_block);

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, NVME_OP_FREE))) {
		_NEs(error_serjio_read_gpt_ents_block, serjio_pd, "Failed (@RV) to execute local nvme command", rv);
		if (op_rsrc->comp) {
			if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
					complete(op_rsrc->comp);
		}
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
		kfree(cb_param);
	}

out:
	NFOUT;
	return rv;
}

static union jblock_md *get_jmdc_block(struct jrange_entry *jrng, unsigned block_idx)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	int jmdc_idx = jrng->rng_rblk + block_idx;
	unsigned long jmdc_offset = jmdc_idx * sizeof(union jblock_md);
	int page_num = jmdc_offset / PAGE_SIZE;
	int page_offset = jmdc_offset % PAGE_SIZE;

	SERJIO_BUG_ON(block_idx >= jrng->rng_rblk + jrng->rng_nblk,
		      bug_get_jmdc_block_inv_block_idx, serjio_pd,
			"Invalid Block Index @BLOCK for Range @JRNL_RNG_IDX",
			block_idx, jrng->range_idx);
	SERJIO_BUG_ON(jmdc_offset >= serjio_pd->jmdc_mem.len,
			bug_get_jmdc_block_inv_jmdc_off, serjio_pd,
			"Invalid JMDC Offset @OFFSET for Block Index @BLOCK, Range @JRNL_RNG_IDX",
			jmdc_offset, block_idx, jrng->range_idx);

	return page_address(serjio_pd->jmdc_mem.pages[page_num]) + page_offset;
}

static union jblock_md *get_jmdc_entry(struct jrange_entry *jrng, unsigned entry_idx)
{
	union jblock_md *entry_start_block = get_jmdc_block(jrng, (entry_idx << jrng->binje_shift));
	union jblock_md *entry_end_block = entry_start_block + (1 << jrng->binje_shift) - 1;

	/* SANITY - entry_idx is not greater than the number of entries in the range */
	BUG_ON(entry_idx >= jrng->n_ents);
	/* SANITY - entry does not cross a page */
	BUG_ON(((unsigned long)entry_start_block & PAGE_MASK) != ((unsigned long)entry_end_block & PAGE_MASK));

	return entry_start_block;
}

static int set_jmdc_entry_data(struct jrange_entry *jrng, int entry_idx, const void *data)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	union jblock_md *jmdc_entry = get_jmdc_entry(jrng, entry_idx);
	int rv = 0;

	_NDs(trace_serjio_set_jmdc_entry_data, serjio_pd, "Writing @DATA to Journal Range: @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX",
		data, jrng->range_idx, entry_idx);

	/* Copy the data to RAM */
	memcpy(jmdc_entry, data, sizeof(union jblock_md) << jrng->binje_shift);

	return rv;
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
static void sync_jmdc_range_to_all_nics(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, int range_idx)
{
	(void)serjio_pd;
	(void)range_idx;
}
#else
static void sync_jmdc_range_to_all_nics(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, int range_idx)
{
	struct jrange_entry *rng = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	const u64 jrange_jmdc_size = rng->rng_nblk * sizeof(union jblock_md);
	u64 jmdc_offset = rng->rng_rblk * sizeof(union jblock_md);
	int page_num = jmdc_offset / PAGE_SIZE;
	int page_offset = jmdc_offset % PAGE_SIZE;
	struct jmdc_mapping *jmdc_mapping, *temp;
	unsigned long flags;
	(void)page_offset;
	(void)page_num;

	/* Now sync with all IB devices (calls ib_dma_sync_single_for_device which doesn't sleep) */
	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_for_each_entry_safe(jmdc_mapping, temp, &serjio_pd->jmdc_mems, link) {
		if (likely(jmdc_mapping->sg_linear_len)) {
			/* Fast-path, mapped sgl is linear wrt range index.
			 * For simplicity, we're just going to sync all pages that the jmdc of the range falls on */
			unsigned ent = jmdc_offset / jmdc_mapping->sg_linear_len;
			unsigned nents = DIV_ROUND_UP(jrange_jmdc_size, jmdc_mapping->sg_linear_len);
			nvmeib_public_ib_dma_sync_sg_for_device(jmdc_mapping->nic_dev->dev->ib_dev,
								jmdc_mapping->jmdc_area_map.mem_table.sgl + ent,
								nents, jmdc_mapping->jmdc_area_map.dma_dir);
		} else {
			nvmeib_mem_sync_map_for_device(&jmdc_mapping->jmdc_area_map,
							jmdc_offset,
							jrange_jmdc_size);
		}
	}
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);
}
#endif //defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)

static bool is_j2d_in_valid_segment(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u64 j2d_start, u64 j2d_end, int *seg_ent, union nvmeib_uuid *seg_id)
{
	struct gpt_entry *gpt_entry;
	bool valid_j2d = false;
	struct seg_tree_entry *seg_entry;

	NFIN;
	if (!(seg_entry = seg_tree_iter_first(&serjio_pd->jrnl_seg_rb_root, j2d_start, j2d_end))) {
		_NDs(trace_serjio_is_j2d_in_valid_segment, serjio_pd,
			 "J2D: [@J2D_START, @J2D_END] does not point to any partition", j2d_start, j2d_end);
		goto out;
	}
	gpt_entry = &serjio_pd->gpt_entry[seg_entry->gpt_idx];
	if (!is_gpt_ent_seg_jrnl(gpt_entry, NULL)){
		_NDs(trace_1_serjio_is_j2d_in_valid_segment, serjio_pd, "J2D: [@J2D_START, @J2D_END] points to partition @UNIQUE_PARTITION_GUID (@GPT_IDX) of type @PARTITION_TYPE_GUID",
			j2d_start, j2d_end, &gpt_entry->unique_partition_guid, seg_entry->gpt_idx,
			&gpt_entry->partition_type_guid);
		goto out;
	} else if (seg_entry->deprecated) {
		_NDs(trace_3_serjio_is_j2d_in_valid_segment, serjio_pd,
			 "J2D: [@J2D_START, @J2D_END] points to @JRNL_SEG_STATUS data segment partition @UNIQUE_PARTITION_GUID (@GPT_IDX)",
			 j2d_start, j2d_end, "DEPRECATED", &gpt_entry->unique_partition_guid, seg_entry->gpt_idx);
		goto out;
	}
	_NDs(trace_2_serjio_is_j2d_in_valid_segment, serjio_pd, "J2D: [@J2D_START, @J2D_END] points to data segment partition @UNIQUE_PARTITION_GUID (@GPT_IDX)",
			j2d_start, j2d_end, &gpt_entry->unique_partition_guid, seg_entry->gpt_idx);
	if (seg_id)
		memcpy(seg_id, &gpt_entry->unique_partition_guid, sizeof(*seg_id));
	if (seg_ent)
		*seg_ent = seg_entry->gpt_idx;
	valid_j2d = true;
out:
	NFOUT;
	return valid_j2d;
}

static void read_jmdc_entry_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int range_idx = op_rsrc->range_idx;
	int binje_shift = op_rsrc->binje_shift;
	int entry_idx = op_rsrc->entry;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	union jblock_md jmdc_entry[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY] = {};
	enum nvmeibs_serjio_jentry_state prev_jentry_state, next_jentry_state;

	(void)result;
	NFIN;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	if (status != 0) {
		_NEs(error_serjio_read_jmdc_entry_cb, serjio_pd, "Error (@STATUS) reading journal entry", status);
		nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << binje_shift);
		set_jmdc_entry_data(jrange_entry, entry_idx, jmdc_entry);
		prev_jentry_state = JENTRY_STATE_CHNG(jrange_entry, entry_idx, JENTRY_IO_ERR, false,
						      JENTRY_STATE_CHNG_REASON_READ_FAIL);
		SERJIO_BUG_ON(prev_jentry_state != JENTRY_UNKNOWN && prev_jentry_state != JENTRY_FREE,
			      bug_read_jmdc_entry_cb_inv_jentry_state, serjio_pd,
				"Entry @JRNL_RNG_ENT_IDX of Range @JRNL_RNG_IDX was "
				"in unexpected state @JENTRY_STATE",
				entry_idx, range_idx, prev_jentry_state);
		goto return_op_rsrc;
	}

	get_nvme_op_rsrc_md(op_rsrc, jmdc_entry, sizeof(jmdc_entry[0]) << op_rsrc->binje_shift);

	_NDs(trace_serjio_read_jmdc_entry_cb, serjio_pd, "Read Journal Metadata @RAW for Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX (Disk LBA: @DISK_BLOCK)",
		 jmdc_entry[0].raw, range_idx, binje_shift, entry_idx, (long unsigned)op_rsrc->nvme_req.disk_block);

	/* Check to see if j2d is valid by looking at the journaled data segment partitions */
	if (!nvmeib_is_jmd_unused_entry(jmdc_entry)) {
		/* Journal Entry is non-empty -
		* If it is part of a free range or does not point to a valid segment,
		* then zero it.
		*/
		u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
		u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
		int chain_err = NVMEIB_JENTRY_CHAIN_OK;
		if (jrange_entry->status == JRANGE_FREE || 
			jrange_entry->status == JRANGE_QUARANTINED ||
			jrange_entry->status == JRANGE_DB_ZERO ||
			(CLEAN_ENTRIES_WITH_INVALID_J2D &&
					(
							nvmeib_is_jmd_trim_val(jmdc_entry) ||
							(chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << binje_shift, &j2d_start, &j2d_end)) != NVMEIB_JENTRY_CHAIN_OK ||
							!is_j2d_in_valid_segment(serjio_pd, j2d_start, j2d_end, NULL, NULL))
					)
			) {
			/* J2D not valid - clear on disk and move to free state */
			_NDs(trace_1_serjio_read_jmdc_entry_cb, serjio_pd, "J2D: [@J2D_START,@J2D_END] in Journal Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX not valid at @PTR. Chain Error @ERROR_STR Chain Error Block @IDX",
				j2d_start, j2d_end, 1 << binje_shift, range_idx, entry_idx, jmdc_entry,
				nvmeib_shared_jentry_md_chain_err_str(chain_err), nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
			if (zero_journal_entry_from_cb(op_rsrc, NULL, NULL, NVME_OP_CB,
				"%s - rng: %u entry: %u status: %s j2d_start: %llu j2d_end: %llu not in valid seg\n", __func__,
				range_idx, entry_idx, nvmeib_shared_serjio_jrange_status_to_str(jrange_entry->status), j2d_start, j2d_end))
				goto return_op_rsrc;
			else
				goto out;
		}
	}

	/* Copy metadata read from the journal to RAM */
	if (set_jmdc_entry_data(jrange_entry, entry_idx, jmdc_entry)) {
		_NEs(error_1_serjio_read_jmdc_entry_cb, serjio_pd, "Failed to set jmdc entry");
		goto return_op_rsrc;
	}

	/* Set state from unknown to dirty or free */
	if (nvmeib_is_jmd_unused_entry(jmdc_entry))
		next_jentry_state = JENTRY_FREE;
	else
		next_jentry_state = JENTRY_SYNCED;

	prev_jentry_state = JENTRY_STATE_CHNG(jrange_entry, entry_idx, next_jentry_state, false,
					      JENTRY_STATE_CHNG_REASON_READ_FREE);

	SERJIO_BUG_ON(prev_jentry_state != JENTRY_UNKNOWN && prev_jentry_state != JENTRY_FREE,
			bug_2_read_jmdc_entry_cb_inv_jentry_state, serjio_pd,
			"Entry @JRNL_RNG_ENT_IDX of Range @JRNL_RNG_IDX was "
			"in unexpected state @JENTRY_STATE",
			entry_idx, range_idx, prev_jentry_state);

return_op_rsrc:
	/* This will also signal the completion that the io thread is waiting for */
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
out:
	NFOUT;
}

static int read_jrnl_entry(struct jrange_entry *jrng, unsigned entry_idx, atomic_t *read_ctr,
						   struct completion *read_comp, nvme_callback_t read_cb, void *cb_param)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	int rv = 0;
	struct nvme_op_rsrc *op_rsrc;

	NFIN;

	if (entry_idx < 0 || entry_idx >= jrng->n_ents) {
		_NEs(error_1_serjio_read_jrnl_entry, serjio_pd,
			 "Invalid journal entry @JRNL_RNG_ENT_IDX for N @BINJE", entry_idx, jrng->binje_shift);
		rv = -EINVAL;
		goto out;
	}

	if (!(op_rsrc = get_free_nvme_op_rsrc_sync(
		serjio_pd, NUM_GET_NVME_RSRC_ATTEMPTS, true))) {
		rv = -ENOMEM;
		goto out;
	}

	op_rsrc->range_idx = jrng->range_idx;
	op_rsrc->binje_shift = jrng->binje_shift;
	op_rsrc->entry = entry_idx;
	op_rsrc->comp_ctr = read_ctr;
	op_rsrc->comp = read_comp;
	op_rsrc->param = cb_param;

	/* Fill the NVME Request */
	init_nvme_op_rsrc_io(op_rsrc, serjio_pd, nvme_cmd_read, false /* hw_blocks */,
			JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, op_rsrc->range_idx, entry_idx),
			JOURNAL_ENTS_TO_BYTES(serjio_pd->di, jrng->binje_shift, 1),
			read_cb, cb_param,
			NULL, 0);

	if (read_ctr)
		atomic_inc(read_ctr);

	/* Submit the request */
	if ((rv = submit_nvme_op_rsrc_to_disk(op_rsrc, NVME_OP_FREE))) {
		_NEs(error_2_serjio_read_jrnl_entry, serjio_pd, "Failed (@RV) to execute local nvme command", rv);
		if (read_ctr)
			atomic_dec(read_ctr);
		return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_ERROR);
	}

	_NDs(trace_serjio_read_jrnl_entry, serjio_pd, "Reading Journal Metadata for Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX",
		 jrng->range_idx, 1 << jrng->binje_shift, entry_idx);

out:
	NFOUT;
	return rv;
}

static void clr_seg_tree_hash(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	/* Clear the jrnl segment tree and hash tables */
	struct hlist_node *t_node __attribute__((unused));
	struct hlist_node *h_node;
	struct seg_tree_entry *iter;
	int i;

	__hash_for_each_safe__(serjio_pd->jrnl_seg_tbl, i, t_node, h_node, iter, link) {
		hash_del(&iter->link);
		seg_tree_remove(iter, &serjio_pd->jrnl_seg_rb_root);
		list_del(&iter->cln_link);
		radix_tree_delete(&serjio_pd->jrnl_seg_radix_root, iter->gpt_idx);
		kfree(iter);
	}
	__hash_for_each_safe__(serjio_pd->del_seg_tbl, i, t_node, h_node, iter, link) {
		hash_del(&iter->link);
		seg_tree_remove(iter, &serjio_pd->del_seg_rb_root);
		list_del(&iter->cln_link);
		kfree(iter);
	}
#if KS_RB_ROOT_CACHED
	BUG_ON(serjio_pd->jrnl_seg_rb_root.rb_root.rb_node != NULL);
	BUG_ON(serjio_pd->del_seg_rb_root.rb_root.rb_node != NULL);
#else
	BUG_ON(serjio_pd->jrnl_seg_rb_root.rb_node != NULL);
	BUG_ON(serjio_pd->del_seg_rb_root.rb_node != NULL);
#endif
	BUG_ON(!list_empty(&serjio_pd->cln_seg_scan_list));
	BUG_ON(!list_empty(&serjio_pd->cln_seg_wait_ret_list));
	BUG_ON(!list_empty(&serjio_pd->cln_seg_wait_scan_list));
}

enum jrnl_segs_gpt_mode {
	GPT_NEW = 0,
	GPT_UPDATE,
	GPT_VERIFY,
};

#ifdef SERJIO_DEBUG_GPT
#pragma GCC push_options
#pragma GCC optimize ("O0")
#endif

static int update_jrnl_segs_from_gpt(struct nvmeibs_serjio_disk_private_data *serjio_pd,
									const struct gpt_entry *new_gpt_ents, unsigned n_new_ents,
									enum jrnl_segs_gpt_mode gpt_mode, bool *schedule_clean)
{
	const struct gpt_entry *new_gpt_ent;
	struct seg_tree_entry *new_seg_entry, *seg_ent_iter;
	char seg_uuid_str[NVMEIB_GID_STR_MAX];
	unsigned i;
	int rv = 0;
	unsigned long flags;
	struct hlist_node *tmp;
	bool seg_deprecated;

	NFIN;
	if (!new_gpt_ents) {
		new_gpt_ents = serjio_pd->gpt_entry;
		n_new_ents = serjio_pd->n_gpt_ents;
	}
	if (gpt_mode != GPT_NEW) {
		/* Check for any deleted/modified segments */
		hash_for_each_safe(serjio_pd->jrnl_seg_tbl, i, tmp, seg_ent_iter, link) {
			bool clean_seg = false;
			bool free_seg_ent = false;
			seg_ent_iter->exists_in_new_gpt = false;
			if (seg_ent_iter->gpt_idx == -1) {
				continue;
			}
			new_gpt_ent = &new_gpt_ents[seg_ent_iter->gpt_idx];
			/* UUIDs in GPT Are little-endian according to EFI Standard */
			nvmeibs_serjio_uuid_le_to_str(seg_uuid_str, new_gpt_ent->unique_partition_guid.b);
			if (is_gpt_ent_seg_jrnl(new_gpt_ent, &seg_deprecated) &&
				strncmp(seg_uuid_str, seg_ent_iter->seg_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
				seg_ent_iter->exists_in_new_gpt = true;
				if (seg_ent_iter->slba != le64_to_cpu(new_gpt_ent->starting_lba) ||
					seg_ent_iter->elba != le64_to_cpu(new_gpt_ent->ending_lba)) {
					_NEs(error_serjio_update_jrnl_segs_seg_moved, serjio_pd,
						 "Segment @SEG_UUID_STR lba range changed from "
						 "[@SLBA_LLONG, @ELBA] to [@SLBA_LLONG, @ELBA]",
						seg_uuid_str, seg_ent_iter->slba, seg_ent_iter->elba,
						le64_to_cpu(new_gpt_ent->starting_lba),
						le64_to_cpu(new_gpt_ent->ending_lba));
					if (gpt_mode == GPT_VERIFY) {
						rv = -EINVAL;
						goto out;
					}
					/* GPT with error has actually been written, crash! */
					BUG_ON(1);
				} else if (seg_deprecated && !seg_ent_iter->deprecated) {
					_NTs(trace_serjio_update_jrnl_segs_from_gpt_seg_deprecated, serjio_pd,
						"Segment @SEG_UUID_STR -> @JRNL_SEG_STATUS", seg_uuid_str, "DEPRECATED");
					if (gpt_mode == GPT_UPDATE) {
						seg_ent_iter->deprecated = true;
						spin_lock_irqsave(&seg_ent_iter->cln_lock, flags);
						if (seg_ent_iter->cln_state == CLN_SEG_IDLE) {
							_NTs(trace_1_serjio_update_jrnl_segs_from_gpt, serjio_pd,
								"Scheduling @JRNL_SEG_STATUS Segment @SEG_UUID_STR for clean",
								"DEPRECATED", seg_ent_iter->seg_uuid_str);
							BUG_ON(seg_ent_iter->wait_rng_cnt != 0);
							BUG_ON(!bitmap_empty(seg_ent_iter->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
							BUG_ON(!list_empty(&seg_ent_iter->cln_link));
							clean_seg = true;
						} else {
							_NTs(trace_3_serjio_update_jrnl_segs_from_gpt, serjio_pd,
								"Already scheduled @JRNL_SEG_STATUS @SEG_UUID_STR for clean",
								"DEPRECATED", seg_ent_iter->seg_uuid_str);
						}
						spin_unlock_irqrestore(&seg_ent_iter->cln_lock, flags);
					}
				}
				if (clean_seg)
					goto sched_seg_clean;
				continue;
			}
			/* Segment not in new GPT */
			spin_lock_irqsave(&seg_ent_iter->cln_lock, flags);
			if (seg_ent_iter->deprecated && seg_ent_iter->cln_state == CLN_SEG_DONE) {
				_NTs(trace_serjio_update_jrnl_segs_from_gpt, serjio_pd,
					"@JRNL_SEG_STATUS Segment @SEG_UUID_STR removed from GPT after clean",
					"DEPRECATED", seg_ent_iter->seg_uuid_str);
				BUG_ON(seg_ent_iter->wait_rng_cnt != 0);
				BUG_ON(!bitmap_empty(seg_ent_iter->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
				BUG_ON(!list_empty(&seg_ent_iter->cln_link));
				free_seg_ent = true;
			} else {
				_NWs(warn_serjio_update_jrnl_segs_from_gpt, serjio_pd,
					 "@JRNL_SEG_STATUS Segment @SEG_UUID_STR unexpectedly removed from GPT",
					(seg_ent_iter->deprecated ? "DEPRECATED" : ""), seg_ent_iter->seg_uuid_str);
				if (gpt_mode == GPT_UPDATE) {
					if (seg_ent_iter->cln_state == CLN_SEG_IDLE) {
					_NTs(trace_2_serjio_update_jrnl_segs_from_gpt, serjio_pd,
						 "Scheduling Segment @SEG_UUID_STR for clean", seg_ent_iter->seg_uuid_str);
						BUG_ON(seg_ent_iter->wait_rng_cnt != 0);
						BUG_ON(!bitmap_empty(seg_ent_iter->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
						BUG_ON(!list_empty(&seg_ent_iter->cln_link));
						clean_seg = true;
					} else {
					_NTs(trace_4_serjio_update_jrnl_segs_from_gpt, serjio_pd,
						 "Waiting for Segment @SEG_UUID_STR clean",
							seg_ent_iter->seg_uuid_str);
					}
				}
			}
			spin_unlock_irqrestore(&seg_ent_iter->cln_lock, flags);
			if (gpt_mode == GPT_UPDATE) {
				/* Remove segment entry from data-structures */
				seg_tree_remove(seg_ent_iter, &serjio_pd->jrnl_seg_rb_root);
				hash_del(&seg_ent_iter->link);
				radix_tree_delete(&serjio_pd->jrnl_seg_radix_root, seg_ent_iter->gpt_idx);
				if (free_seg_ent) {
					/* Segment has been cleaned already - free it */
					kfree(seg_ent_iter);
					continue;
				}
				/* Segment has not been been cleaned move to deleted data-structures */
				seg_ent_iter->gpt_idx = -1;
				seg_ent_iter->seg_delete = true;
				seg_tree_insert(seg_ent_iter, &serjio_pd->del_seg_rb_root);
				hash_add(serjio_pd->del_seg_tbl, &seg_ent_iter->link, hash_uuid_str(seg_ent_iter->seg_uuid_str));
				if (clean_seg)
					goto sched_seg_clean;
			}
			continue;

sched_seg_clean:
			/* Schedule the segment for cleaning */
			BUG_ON(!(gpt_mode == GPT_UPDATE && clean_seg));
			spin_lock_irqsave(&seg_ent_iter->cln_lock, flags);
			seg_ent_iter->cln_state = CLN_SEG_WAIT_SCAN;
			list_add_tail(&seg_ent_iter->cln_link, &serjio_pd->cln_seg_wait_scan_list);
			spin_unlock_irqrestore(&seg_ent_iter->cln_lock, flags);
			if (schedule_clean)
				*schedule_clean = true;
		}
	}

	/* Check for any new segments */
	for (i = 0, new_gpt_ent = new_gpt_ents; i < n_new_ents; i++, new_gpt_ent++) {
		u64 new_slba = le64_to_cpu(new_gpt_ent->starting_lba);
		u64 new_elba = le64_to_cpu(new_gpt_ent->ending_lba);
		if (!is_gpt_ent_seg_jrnl(new_gpt_ent, &seg_deprecated))
			continue;
		nvmeibs_serjio_uuid_le_to_str(seg_uuid_str, new_gpt_ent->unique_partition_guid.b);

		if (gpt_mode != GPT_NEW) {
			bool seg_found = false;
			/* Check if segment is already in the hash table/tree */
			hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_ent_iter, link,
									hash_uuid_str(seg_uuid_str)) {
				if (strncmp(seg_uuid_str, seg_ent_iter->seg_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
					/* Already in the tree */
					if ((unsigned)seg_ent_iter->gpt_idx != i) {
						_NEs(error_serjio_update_jrnl_segs_from_gpt_seg_moved, serjio_pd,
							 "Segment @SEG_UUID_STR unexpectedly moved from entry @GPT_IDX to @GPT_IDX",
						   seg_ent_iter->seg_uuid_str, seg_ent_iter->gpt_idx, i);
						if (gpt_mode == GPT_VERIFY) {
							rv = -EINVAL;
							goto out;
						}
						/* GPT with error has actually been written, crash! */
						BUG_ON(1);
					}
					if (seg_ent_iter->slba != new_slba || seg_ent_iter->elba != new_elba) {
						_NEs(error_serjio_update_jrnl_segs_from_gpt_seg_moved2, serjio_pd,
							 "Segment @SEG_UUID_STR lba range changed from [@SLBA_LLONG, @ELBA] to [@SLBA_LLONG, @ELBA]",
							seg_uuid_str, seg_ent_iter->slba, seg_ent_iter->elba, new_slba, new_elba);
						BUG_ON(1);
						if (gpt_mode == GPT_VERIFY) {
							rv = -EINVAL;
							goto out;
						}
						/* GPT with error has actually been written, crash! */
						BUG_ON(1);
					}
					seg_found = true;
					break;
				}
			}
			if (seg_found)
				continue;
			/* GPT Segment not found in current data structures, is new */
			_NTs(trace_serjio_update_jrnl_segs_from_gpt_new_seg, serjio_pd,
				 "New segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA] added to GPT", seg_uuid_str, new_slba, new_elba);
			if (seg_deprecated) {
				_NWs(trace_serjio_update_jrnl_segs_from_gpt_new_seg_deprecated, serjio_pd,
					 "New segment @SEG_UUID_STR is already @JRNL_SEG_STATUS", seg_uuid_str, "DEPRECATED");
			}
			/* Check to see if there are no overlaps */
			for (seg_ent_iter = seg_tree_iter_first(
					&serjio_pd->jrnl_seg_rb_root, new_slba, new_elba);
					seg_ent_iter != NULL; seg_ent_iter = seg_tree_iter_next(
						seg_ent_iter, new_slba, new_elba)) {
				/* Make sure seg exists in new GPT otherwise it is not an overlap */
				if (seg_ent_iter->exists_in_new_gpt) {
					if (seg_ent_iter->deprecated || seg_ent_iter->seg_delete) {
						_NTs(trace_serjio_update_jrnl_segs_from_gpt_seg_overlaps, serjio_pd,
						"New segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA] overlaps with "
						"existing @JRNL_SEG_STATUS segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA]",
						seg_uuid_str, new_slba, new_elba, seg_ent_iter->deprecated ? "DEPRECATED" : "DELETED",
						seg_ent_iter->seg_uuid_str, seg_ent_iter->slba, seg_ent_iter->elba);
					} else {
						_NEs(error_serjio_update_jrnl_segs_from_gpt_seg_overlaps, serjio_pd,
							"New segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA] overlaps with "
							"existing live segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA]",
							seg_uuid_str, new_slba, new_elba,
							seg_ent_iter->seg_uuid_str, seg_ent_iter->slba, seg_ent_iter->elba);
						if (gpt_mode == GPT_VERIFY) {
							rv = -EINVAL;
							goto out;
						}
						/* GPT with error has actually been written, crash! */
						BUG_ON(1);
					}
				}
			}
		}
		if (gpt_mode == GPT_VERIFY)
			continue;

		/* Add segment */
		if (!(new_seg_entry = kzalloc(sizeof(*new_seg_entry), GFP_ATOMIC))) {
			_NEs(error_serjio_update_jrnl_segs_from_gpt, serjio_pd, "Memory allocation error");
			rv = -ENOMEM;
			goto out;
		}
		memcpy(&new_seg_entry->seg_uuid, &new_gpt_ent->unique_partition_guid,
				sizeof(new_seg_entry->seg_uuid));
		memcpy(&new_seg_entry->seg_type, &new_gpt_ent->partition_type_guid,
				sizeof(new_seg_entry->seg_type));
		memcpy(new_seg_entry->seg_uuid_str, seg_uuid_str, NVMEIB_GID_STR_MAX);
		new_seg_entry->slba = le64_to_cpu(new_gpt_ent->starting_lba);
		new_seg_entry->elba = le64_to_cpu(new_gpt_ent->ending_lba);
		new_seg_entry->gpt_idx = i;
		new_seg_entry->deprecated = seg_deprecated;

		seg_tree_insert(new_seg_entry, &serjio_pd->jrnl_seg_rb_root);
		hash_add(serjio_pd->jrnl_seg_tbl, &new_seg_entry->link,
					hash_uuid_str(new_seg_entry->seg_uuid_str));
		radix_tree_insert(
			&serjio_pd->jrnl_seg_radix_root, new_seg_entry->gpt_idx, new_seg_entry);
		INIT_LIST_HEAD(&new_seg_entry->cln_link);
		spin_lock_init(&new_seg_entry->cln_lock);
		new_seg_entry->cln_state = CLN_SEG_IDLE;
	}

out:
	NFOUT;
	return rv;
}

/* Works around the TOMA GPT hacks to get around the SuperMicro BIOS problem
 * - For GPT created by 1.3 TOMA, the GPT Table contains 8K entries, the header says there are only 128 and it only uses and calcs CRC over the 128
 * - For GPT created by 2.0 TOMA, the GPT Table contains 8K entries, the header says there are only 128 and it uses and calcs CRC over the whole 8K
 *
 * To tell the difference between them, we check both CRCs against the header. This is pretty hacky, but works
 */
static int gpt_hdr_num_parts(struct nvmeibs_serjio_disk_private_data *serjio_pd,
							 bool primary, struct gpt_header *gpt_hdr, struct gpt_entry *gpt_ents, size_t gpt_ents_sz)
{
	int num_ents_by_hdr = le32_to_cpu(gpt_hdr->num_partition_entries);
	int num_ents_by_offset;
	int rv;
	size_t gpt_ents_sz_by_hdr = num_ents_by_hdr * sizeof(*gpt_ents);
	size_t gpt_ents_sz_by_offset;
	__le32 crc_over_hdr_num_ents = 0;
	__le32 crc_over_ents = 0;

	if (primary) {
		num_ents_by_offset = DISK_LBAS_TO_BYTES(serjio_pd->di,
														le64_to_cpu(gpt_hdr->first_usable_lba) - le64_to_cpu(gpt_hdr->partition_entry_lba)) /
														le32_to_cpu(gpt_hdr->sizeof_partition_entry);
	} else {
		num_ents_by_offset = DISK_LBAS_TO_BYTES(serjio_pd->di,
														le64_to_cpu(gpt_hdr->my_lba) - le64_to_cpu(gpt_hdr->partition_entry_lba)) /
														le32_to_cpu(gpt_hdr->sizeof_partition_entry);
	}
	gpt_ents_sz_by_offset = num_ents_by_offset * sizeof(*gpt_ents);

	if (num_ents_by_offset < num_ents_by_hdr) {
		_NEs(err_serjio_gpt_hdr_num_parts_inv_ents_hdr_sz, serjio_pd,
			 "Mismatch between GPT Header My LBA: @LBA_LLONG, First Usable LBA: @LBA_LLONG Partition Entry LBA: @LBA_LLONG and GPT Number of Entries @NUMBER_OF_PARTITION_ENTRIES",
			le64_to_cpu(gpt_hdr->my_lba), le64_to_cpu(gpt_hdr->first_usable_lba),
			 le64_to_cpu(gpt_hdr->partition_entry_lba), num_ents_by_hdr);
		rv = -EINVAL;
		goto out;
	}

	/* If we haven't read the entries yet, then return the maximum */
	if (!gpt_ents) {
		rv = num_ents_by_offset;
		goto out;
	}

	/* Calculate CRC over header entries buffer */
	crc_over_hdr_num_ents = efi_crc32(gpt_ents, gpt_ents_sz_by_hdr);

	/* If the size by offsets and CRC matches header, there is no workaround */
	if (gpt_ents_sz == gpt_ents_sz_by_offset &&
		gpt_ents_sz == ALIGN(gpt_ents_sz_by_hdr, (1 << nvmeibs_disk_info_get_block_shift(serjio_pd->di))) &&
		gpt_hdr->partition_entry_array_crc32 == crc_over_hdr_num_ents) {
		_NTs(trace_serjio_gpt_hdr_num_parts_no_workaround, serjio_pd,
			 "Correct GPT Table - number of GPT entries @NUMBER_OF_PARTITION_ENTRIES size @SIZE_T",
				num_ents_by_hdr, gpt_ents_sz_by_hdr);
		rv = num_ents_by_hdr;
		goto out;
	}

	/* If we are here, either we have a 2.0 workaround table or a 1.3 workaround table or there is a CRC or length error */

	/* Check for 2.0 workaround table i.e. number of entries is determined by offsets */
	if (gpt_ents_sz >= gpt_ents_sz_by_offset) {
		crc_over_ents = efi_crc32(gpt_ents, gpt_ents_sz_by_offset);
		if (gpt_hdr->partition_entry_array_crc32 == crc_over_ents) {
			_NTs(trace_serjio_gpt_hdr_num_parts_2_0_tbl, serjio_pd,
				"2.0 GPT Table Workaround - number of GPT entries @NUMBER_OF_PARTITION_ENTRIES size @SIZE_T",
				num_ents_by_offset, gpt_ents_sz_by_offset);
			rv = num_ents_by_offset;
			goto out;
		}
	}

	/* Failed, check for 1.3 workaround table i.e. Number of entries is determined by header */
	if (gpt_ents_sz >= gpt_ents_sz_by_hdr) {
		if (gpt_hdr->partition_entry_array_crc32 == crc_over_hdr_num_ents) {
			_NTs(trace_serjio_gpt_hdr_num_parts_1_3_tbl, serjio_pd,
				 "1.3 GPT Table Workaround - number of GPT entries @NUMBER_OF_PARTITION_ENTRIES size @SIZE_T",
		num_ents_by_hdr, gpt_ents_sz_by_hdr);
			rv = num_ents_by_hdr;
			goto out;
		}
	}

	_NEs(err_serjio_gpt_hdr_num_parts_crc_error, serjio_pd,
		 "GPT Size / CRC Error calculated @CALC_GPT_HDR_CRC32 over @SIZE_T bytes (for 2.0 Table) and @CALC_GPT_HDR_CRC32 over @SIZE_T bytes (for 1.3 Table), header has @CALC_GPT_HDR_CRC32",
		 le32_to_cpu(crc_over_ents), gpt_ents_sz_by_offset,
		 le32_to_cpu(crc_over_hdr_num_ents), gpt_ents_sz_by_hdr,
		 le32_to_cpu(gpt_hdr->partition_entry_array_crc32));

	rv = -EINVAL;

out:
	return rv;
}

static int read_gpt(struct nvmeibs_serjio_disk_private_data *serjio_pd,
					bool primary, struct gpt_header *gpt_hdr,
					struct gpt_entry **gpt_ents, size_t *gpt_ents_sz, unsigned *n_gpt_ents)
{
	atomic_t read_ctr;
	DECLARE_COMPLETION_ONSTACK(read_comp);
	int rv;
	u32 calc_gpt_hdr_crc32, gpt_hdr_crc32;
	u64 gpt_ents_num_lba, gpt_ents_first_lba;
	int gpt_ents_per_lba;
	int i;

	if (*gpt_ents) {
		_NEs(error_serjio_read_gpt_gpt_ents_not_null, serjio_pd,
			 "gpt_ents @PTR is not null", *gpt_ents);
		rv = -EINVAL;
		goto out;
	}

	/* Read the GPT Header */
	if ((rv = read_gpt_hdr(serjio_pd, primary, &read_comp, gpt_hdr))) {
		_NEs(error_serjio_read_gpt, serjio_pd, "Error (@RV) reading GPT", rv);
		goto out;
	}
	wait_for_completion(&read_comp);

	if (le32_to_cpu(gpt_hdr->header_size) != sizeof(*gpt_hdr)) {
		_NEs(error_2_serjio_read_gpt, serjio_pd, "GPT header has invalid size @SIZE",
			le32_to_cpu(gpt_hdr->header_size));
		rv = -EINVAL;
		goto out;
	}

	/* Save and zero the crc before calculating */
	gpt_hdr_crc32 = gpt_hdr->header_crc32;
	gpt_hdr->header_crc32 = 0;
	calc_gpt_hdr_crc32 = efi_crc32(gpt_hdr, sizeof(*gpt_hdr));
	/* Restore crc */
	gpt_hdr->header_crc32 = gpt_hdr_crc32;

	if (le64_to_cpu(gpt_hdr->signature) != GPT_HEADER_SIGNATURE ||
		le32_to_cpu(gpt_hdr->revision) != GPT_HEADER_REVISION_V1 ||
		gpt_hdr->header_crc32 != calc_gpt_hdr_crc32) {
		_NEs(error_1_serjio_read_gpt, serjio_pd, "GPT has invalid sig (@SIGNATURE),"
		" rev (@REVISION) or crc (@HEADER_CRC32 != @CALC_GPT_HDR_CRC32)",
			le64_to_cpu(gpt_hdr->signature), le32_to_cpu(gpt_hdr->revision),
			gpt_hdr->header_crc32, calc_gpt_hdr_crc32);
		rv = -EINVAL;
		goto out;
	}

	*n_gpt_ents = gpt_hdr_num_parts(serjio_pd, primary, gpt_hdr, NULL, 0);

	if (*n_gpt_ents <= 0) {
		_NEs(error_3_serjio_read_gpt, serjio_pd, "Error @RV processing GPT Error", *n_gpt_ents);
		rv = (int)(*n_gpt_ents) ?: -ENOENT;
		goto out;
	}
	*gpt_ents_sz = *n_gpt_ents * sizeof(struct gpt_entry);

	if (!(*gpt_ents = vzalloc(*gpt_ents_sz))) {
		_NEs(error_4_serjio_read_gpt, serjio_pd, "Memory allocation error");
		rv = -ENOMEM;
		goto out;
	}

	/* Calculate disk lbas needed to read all the entries */
	gpt_ents_per_lba = DISK_LBAS_TO_BYTES(serjio_pd->di, 1) /
		le32_to_cpu(serjio_pd->gpt_hdr.sizeof_partition_entry);
		gpt_ents_num_lba = DIV_ROUND_UP(*n_gpt_ents, gpt_ents_per_lba);
	gpt_ents_first_lba = le64_to_cpu(gpt_hdr->partition_entry_lba);

	_NTs(trace_1_serjio_read_gpt, serjio_pd,
		 "Reading GPT from LBA: @GPT_LBA for Disk: @DISK_GUID -"
		 " Entries: @JRNL_RNG_ENT_IDX (LBA: @SLBA_LLONG - @ELBA)",
		le64_to_cpu(gpt_hdr->my_lba), &gpt_hdr->disk_guid,
		 *n_gpt_ents, gpt_ents_first_lba,
		gpt_ents_first_lba + gpt_ents_num_lba - 1);

	/* Read GPT Partition Entries */
	atomic_set(&read_ctr, 1);
	for (i = 0; i < (int)gpt_ents_num_lba; i++) {
		atomic_inc(&read_ctr);
		if ((rv = read_gpt_ents_block(serjio_pd, primary, gpt_hdr, *gpt_ents, *n_gpt_ents, i, &read_ctr, &read_comp))) {
			_NEs(error_5_serjio_io_rd_gpt_fn, serjio_pd, "Error (@RV) reading GPT entries LBA @LBA", rv, i);
			atomic_dec(&read_ctr);
			rv = -EIO;
			goto wait_comp;
		}
	}
	rv = 0;

wait_comp:
	if (atomic_dec_return(&read_ctr) > 0)
		wait_for_completion(&read_comp);

	/* Update number of partitions now that we have the entries (also checks the CRC) */
	if ((rv = gpt_hdr_num_parts(serjio_pd, primary, gpt_hdr, *gpt_ents, *gpt_ents_sz)) <= 0) {
		_NEs(error_6_serjio_io_rd_gpt_fn, serjio_pd, "Error (@RV) calculating number of partitions\n", rv);
		rv = rv ?: -ENOENT;
	}
	else {
		*n_gpt_ents = rv;
	}

	if (rv < 0) {
		vfree(*gpt_ents);
		*gpt_ents = NULL;
		*gpt_ents_sz = 0;
	}

out:
	return rv;
}

static const struct gpt_entry gpt_zero_entry = {.starting_lba = 0};

static void map_jmdc_at_all_nics(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	struct nvmeibs_dev *nis_dev;
	struct list_head *devices;
	int rv;

	devices = nvmeibs_serjio_get_devices(NULL);
	list_for_each_entry(nis_dev, devices, nvmeibs_dev_list_n) {
		if ((rv = map_jmdc_at_device(nis_dev, serjio_pd))) {
			_NTs(trace_serjio_map_jmdc_at_all_nics, serjio_pd,
			     "Failed (@RV) to map JMD Cache to NIC @IB_DEV_NAME",
			     rv, nis_dev->dev->ib_dev->name);
		}
	}

	nvmeibs_serjio_put_devices();
}

static void disconnect_all_allocated_clients(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	struct jrange_entry *jrng;
	int bkt, rv;
	hash_for_each(serjio_pd->jranges_alloc_tbl.reserved_ranges, bkt, jrng, link) {
		if (jrng->status == JRANGE_ALLOCATED) {
			_NTs(trace_disconnect_all_allocated_clients, serjio_pd,
			     "Disconnecting client @CID_LLONG from range @JRNL_RNG_IDX",
				jrng->client_id, jrng->range_idx);
			if ((rv = nvmeibs_remove_cid_clients(jrng->client_id,
					 NVMEIBS_LOGOUT_REASON_SERJIO_ERROR) < 0)) {
				_NEs(err_disconnect_all_allocated_clients, serjio_pd, 
				     "nvmeibs_remove_cid_clients failed (@RV) for client @CID_LLONG\n", 
				     rv, jrng->client_id);
			}
		}
	}
}

static void clear_all_gpt_jrnl_segs(struct nvmeibs_serjio_disk_private_data *serjio_pd)
{
	struct seg_tree_entry *seg_ent_iter;
	struct hlist_node *tmp;
	unsigned i;
	hash_for_each_safe(serjio_pd->jrnl_seg_tbl, i, tmp, seg_ent_iter, link) {
		seg_tree_remove(seg_ent_iter, &serjio_pd->jrnl_seg_rb_root);
		hash_del(&seg_ent_iter->link);
		radix_tree_delete(&serjio_pd->jrnl_seg_radix_root, seg_ent_iter->gpt_idx);
		kfree(seg_ent_iter);
	}
	hash_for_each_safe(serjio_pd->del_seg_tbl, i, tmp, seg_ent_iter, link) {
		seg_tree_remove(seg_ent_iter, &serjio_pd->jrnl_seg_rb_root);
		hash_del(&seg_ent_iter->link);
		radix_tree_delete(&serjio_pd->jrnl_seg_radix_root, seg_ent_iter->gpt_idx);
		kfree(seg_ent_iter);
	}
}

static void serjio_go_to_err_state(struct nvmeibs_serjio_disk_private_data *serjio_pd,
				   enum nvmeibs_serjio_state err_state)
{
	/* Put SERJIO in Error State */
	serjio_state_exch(serjio_pd, err_state);

	/* Disconnect all the clients. They can't use the journal while SERJIO is in Error state */
	disconnect_all_allocated_clients(serjio_pd);

	/* Clear the data-structures for all Journal Segments in the GPT */
	clear_all_gpt_jrnl_segs(serjio_pd);

	/* Reset all the ranges to invalid. They will be re-initialised after the GPT is repaired */
	init_jrange_lists(serjio_pd, false);
}

static DECLARE_IO_WQ_FN(io_rd_gpt_fn)
{
	int i, zero_ent_start;
	int rv = 0;
	DECLARE_COMPLETION_ONSTACK(read_comp);
	enum nvmeibs_serjio_state init_state = serjio_state_get(serjio_pd);
	enum nvmeibs_serjio_state fini_state_err = SERJIO_ERR_GPT;
	bool schedule_clean = false;
	struct gpt_entry *gpt_entry;
	bool seg_deprecated;
	char uuid_str[NVMEIB_GID_STR_MAX];

	NFIN;
	(void)param;
	if (init_state == SERJIO_INIT) {
		BUG_ON(serjio_state_cmp_exch(
			serjio_pd, init_state, SERJIO_RD_GPT) != init_state);
		_NTs(io_rd_gpt_fn_t1, serjio_pd, "Reading GPT");
	} else if (init_state == SERJIO_GPT_INIT) {
		_NTs(io_rd_gpt_fn_t6, serjio_pd, "Checking GPT before Init DB/Journal");
	} else if (init_state == SERJIO_GPT_UPDATE) {
		BUG_ON(serjio_pd->gpt_upd_state != SERJIO_GPT_POST_WRITE_DONE
			&& serjio_pd->gpt_upd_state != SERJIO_GPT_UPDATE_CANCELLED);
		BUG_ON(!serjio_pd->pend_gpt_hdr || !serjio_pd->pend_gpt_ents || !serjio_pd->pend_gpt_ents_sz || !serjio_pd->n_pend_gpt_ents);
		_NTs(io_rd_gpt_fn_t2, serjio_pd, "Re-reading GPT");
	} else if (SERJIO_HALTING(init_state)) {
		_NTs(io_rd_gpt_fn_t3, serjio_pd, "SERJIO in shut-down state @SERJIO_STATE - Cancelling GPT Read", init_state);
		rv = -ECANCELED;
		goto out;
	} else {
		_NTs(io_rd_gpt_fn_t4, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", init_state);
		BUG_ON(1);
	}
	
	if (nvmeibs_serjio_fail_next_gpt_update && init_state == SERJIO_GPT_UPDATE) {
		_NTs(io_rd_gpt_fn_t5, serjio_pd, "SERJIO TEST - Failing GPT Update");
		nvmeibs_serjio_fail_next_gpt_update = false;
		rv = -1;
		goto out;
	}

	down_write(&serjio_pd->gpt_rwsem);
	if (serjio_pd->gpt_entry) {
		vfree(serjio_pd->gpt_entry);
		serjio_pd->gpt_entry = NULL;
	}

	/* Read the Primary GPT */
	if ((rv = read_gpt(serjio_pd, true, &serjio_pd->gpt_hdr,
		&serjio_pd->gpt_entry, &serjio_pd->gpt_ents_sz, &serjio_pd->n_gpt_ents)) < 0) {
		_NEs(error_serjio_io_rd_gpt_fn, serjio_pd, "Error (@RV) reading GPT", rv);
		goto unlock;
	}

	for (i = 0, zero_ent_start = -1, gpt_entry = serjio_pd->gpt_entry; i < (int)serjio_pd->n_gpt_ents; i++, gpt_entry++) {
		char part_name[80] = "";
		int c;

		/* Don't full log the zero entries */
		if (memcmp(&gpt_zero_entry, gpt_entry, sizeof(*gpt_entry)) == 0) {
			if (zero_ent_start == -1)
				zero_ent_start = i;
			continue;
		} else if (zero_ent_start >= 0) {
			_NTs(trace_serjio_io_gpt_rd_fn_print_ents_zero, serjio_pd,
				 "GPT Entries [@GPT_IDX - @GPT_IDX): 0", zero_ent_start, i);
			zero_ent_start = -1;
		}

		for (c = 0; c < (int)ARRAY_SIZE(gpt_entry->partition_name); c++)
			part_name[c] = gpt_entry->partition_name[c] & 0xff;

		_NTs(trace_serjio_io_rd_gpt_fn_print_ents, serjio_pd,
			"GPT Entry [@GPT_IDX]: Type GUID: @PARTITION_TYPE_GUID"
			" Unique GUID: @UNIQUE_PARTITION_GUID SLBA: @SLBA_LLONG ELBA: @ELBA"
			" Attributes: @GPT_PARTITION_ATTRIBUTES Name: @GPT_PARTITION_NAME",
			i, &gpt_entry->partition_type_guid, &gpt_entry->unique_partition_guid,
			le64_to_cpu(gpt_entry->starting_lba), le64_to_cpu(gpt_entry->ending_lba),
			le64_to_cpu(gpt_entry->attributes), part_name);

		if (is_gpt_ent_jrnl(gpt_entry)) {
			/* Found Journal partition */
			serjio_pd->disk_ranges.journal.part_idx = i;
			serjio_pd->disk_ranges.journal.lba = le64_to_cpu(gpt_entry->starting_lba);
			serjio_pd->disk_ranges.journal.len_nlbas =
				le64_to_cpu(gpt_entry->ending_lba) - serjio_pd->disk_ranges.journal.lba + 1;
			serjio_pd->disk_ranges.journal.part_uuid = gpt_entry->unique_partition_guid;
			_NTs(trace_serjio_read_gpt_ents_block_cb, serjio_pd, "Found journal partition [@LBA_LLONG - @LEN_NLBAS)",
				serjio_pd->disk_ranges.journal.lba, serjio_pd->disk_ranges.journal.lba +
				serjio_pd->disk_ranges.journal.len_nlbas);
		}
		else if (is_gpt_ent_serjio_db(gpt_entry)) {
			/* Found SERJIO DB partition */
			serjio_pd->disk_ranges.db.part_idx = i;
			serjio_pd->disk_ranges.db.lba = le64_to_cpu(gpt_entry->starting_lba);
			serjio_pd->disk_ranges.db.len_nlbas =
				le64_to_cpu(gpt_entry->ending_lba) - serjio_pd->disk_ranges.db.lba + 1;
			serjio_pd->disk_ranges.db.part_uuid = gpt_entry->unique_partition_guid;
			_NTs(trace_1_serjio_read_gpt_ents_block_cb, serjio_pd, "Found SERJIO DB partition [@LBA_LLONG - @LEN_NLBAS)",
				serjio_pd->disk_ranges.db.lba, serjio_pd->disk_ranges.db.lba +
				serjio_pd->disk_ranges.db.len_nlbas);
		}
		else if (is_gpt_ent_seg_jrnl(gpt_entry, &seg_deprecated)) {
			/* Journalled segment */
			_NTs(trace_2_serjio_read_gpt_ents_block_cb, serjio_pd, "Found @JRNL_SEG_STATUS journalled segment partition @PARTITION [@GPT_LBA - @GPT_LBA)",
				seg_deprecated ? "DEPRECATED" : "",
				nvmeibs_serjio_uuid_le_to_str(uuid_str, gpt_entry->unique_partition_guid.b),
				le64_to_cpu(gpt_entry->starting_lba), le64_to_cpu(gpt_entry->ending_lba));
		} else if (is_gpt_ent_seg_no_jrnl(gpt_entry)) {
			/* Non-journalled Segment */
			_NTs(trace_3_serjio_read_gpt_ents_block_cb, serjio_pd, "Found non-journalled segment partition @PARTITION [@GPT_LBA - @GPT_LBA)",
				nvmeibs_serjio_uuid_le_to_str(uuid_str, gpt_entry->unique_partition_guid.b),
				le64_to_cpu(gpt_entry->starting_lba), le64_to_cpu(gpt_entry->ending_lba));
			_NTs(trace_4_serjio_read_gpt_ents_block_cb, serjio_pd, "Found journalled segment @SEG under deletion [@GPT_LBA - @GPT_LBA)",
				nvmeibs_serjio_uuid_le_to_str(uuid_str, gpt_entry->unique_partition_guid.b),
				le64_to_cpu(gpt_entry->starting_lba), le64_to_cpu(gpt_entry->ending_lba));
		}
	}
	if (zero_ent_start >= 0) {
		_NTs(trace_serjio_io_gpt_rd_fn_print_ents_zero_2, serjio_pd,
			 "GPT Entries [@GPT_IDX - @GPT_IDX): 0", zero_ent_start, i);
	}

	if (!serjio_pd->disk_ranges.journal.len_nlbas) {
		_NWs(warn_serjio_io_rd_gpt_fn, serjio_pd, "GPT is missing Journal Partition");
		rv = -ENOENT;
		fini_state_err = SERJIO_ERR_NO_JRNL;
		goto unlock;
	}

	if (!serjio_pd->disk_ranges.db.len_nlbas) {
		_NWs(warn_1_serjio_io_rd_gpt_fn, serjio_pd, "GPT is missing SERJIO DB Partition");
		rv = -ENOENT;
		fini_state_err = SERJIO_ERR_NO_DB;
		goto unlock;
	}

	if (init_state == SERJIO_GPT_UPDATE) {
		if (serjio_pd->gpt_upd_state != SERJIO_GPT_UPDATE_CANCELLED) {
			SERJIO_BUG_ON(serjio_pd->n_gpt_ents != serjio_pd->n_pend_gpt_ents, 
				      err_serjio_io_rd_gpt_fn_n_ents_chng, serjio_pd, 
					"Num partition entries changed from pending GPT update");
			SERJIO_BUG_ON(memcmp(serjio_pd->gpt_entry, serjio_pd->pend_gpt_ents, 
					     serjio_pd->n_gpt_ents * sizeof(struct gpt_entry)) != 0, 
						err_serjio_io_rd_gpt_fn_ents_chng, serjio_pd,
				      "Partition entries changed from pending GPT update");
		}

		kfree(serjio_pd->pend_gpt_hdr);
		serjio_pd->pend_gpt_hdr = NULL;
		vfree(serjio_pd->pend_gpt_ents);
		serjio_pd->pend_gpt_ents = NULL;
		serjio_pd->n_pend_gpt_ents = 0;

		/* Update Journal Segment Data Structures */
		if ((rv = update_jrnl_segs_from_gpt(serjio_pd, NULL, 0, GPT_UPDATE, &schedule_clean)))
			goto unlock;

		up_write(&serjio_pd->gpt_rwsem);

		/* Move back to ready state */
		BUG_ON(serjio_state_cmp_exch(
			serjio_pd, SERJIO_GPT_UPDATE, SERJIO_READY) != SERJIO_GPT_UPDATE);
		serjio_pd->gpt_upd_state = SERJIO_GPT_UPDATE_IDLE;

		if (schedule_clean) {
			/* Schedule clean of deleted segments (run in-place so we don't allocate any more ranges) */
			rv = run_on_io_wq(serjio_pd, io_cln_jrnl_disk_rng_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG);
		}
		goto out;
	}

	if ((rv = serjio_init_calc_num_jranges(serjio_pd))) {
		goto unlock;
	}

	/* Build Journal Segment Data Structures */
	if ((rv = update_jrnl_segs_from_gpt(serjio_pd, NULL, 0, GPT_NEW, NULL)))
		goto unlock;

#if ALLOC_JMDC_AFTER_RD_GPT
	if (init_state == SERJIO_INIT || init_state == SERJIO_GPT_INIT) {
		/* Allocate the JMDC based on the size of the partitions */
		if ((rv = alloc_jmdc(serjio_pd)) < 0) {
			fini_state_err = SERJIO_ERR_JMDC_OOM;
			goto out;
		}
		map_jmdc_at_all_nics(serjio_pd);
	}
#endif

	up_write(&serjio_pd->gpt_rwsem);

	/* Schedule the next work */
	if (init_state == SERJIO_GPT_INIT) {
		/* [NVMESH-4781]: Delay the GPT Write Disk Completion until SERJIO Inits the Journal */
		rv = run_on_io_wq(serjio_pd, io_init_jrnl_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_INIT_JRNL);
	} else {
		rv = run_on_io_wq(serjio_pd, io_rd_db_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_DB);
	}
	
	goto out;

unlock:
	up_write(&serjio_pd->gpt_rwsem);

out:
	if (rv < 0 && rv != -EINPROGRESS && rv != -ECANCELED) {
		/* Failed to read the GPT - Put SERJIO in Error State */
		serjio_go_to_err_state(serjio_pd, fini_state_err);
	}

	NFOUT;
	return rv;
}

#ifdef SERJIO_DEBUG_GPT
#pragma GCC pop_options
#endif

static int free_jrnl_rng(struct jrange_entry *rng, struct completion *comp, atomic_t *ctr);

static int check_jrnl_rng_blk(struct nvmeibs_serjio_disk_private_data *serjio_pd, u32 rng_nlba, u32 rng_nblk)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *rng;
	int i, rv = 0;
	u32 rng_rlba = 0;
	u32 rng_rblk = 0;

	for (i = 0, rng = jranges_alloc_tbl->ranges; i < (int)jranges_alloc_tbl->num_ranges; i++, rng++) {
		if (rng_rlba < serjio_pd->disk_ranges.journal.len_nlbas) {
			unsigned exp_n_ents;
			/* [NVMESH-5279]: There are 2 possibilities for ranges with index < NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES
			 * 1: SERJIO DB was init before the fix
			 *	==> Ranges 0, 1, 2 have status JRANGE_FREE, and are allocated on disk 
			 * 		i.e. rng->rng_rlba and rng->rng_nlba are valid and must be taken into account.
			 * 2: SERJIO DB was init after the fix 
			 * 	==> Ranges 0, 1, 2 have status JRANGE_INVALID, but are not allocated on disk 
			 * 		i.e. rng->rng_rlba == NVMEIB_EC_INVALID_JOURNAL_RLBA and rng->rng_rblk == NVMEIB_EC_INVALID_JOURNAL_RBLK
			 */
			if (rng->status == JRANGE_DB_ERR || rng->status == JRANGE_DB_ZERO || rng->status == JRANGE_INVALID)
			{
				if (rng->status == JRANGE_INVALID) {
					/* [NVMESH-5279]: Option 2: Range is invalid and not stored on disk, skip increment of rng_rlba, rng_rblk */
					if (rng->range_idx < NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) {
						_NTs(trace_check_jrnl_rng_blk_skip_inv_jri, serjio_pd,
							"Range @JRNL_RNG_IDX is invalid for client (< NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES @JRNL_RNG_IDX) and not allocated on disk. Skipping",
							rng->range_idx, NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES);
						/* Skip increment of rng_rlba, rng_rblk */
						continue;
					} else {
						_NTs(err_check_jrnl_rng_blk_unexpected_inv, serjio_pd,
						     "Range @JRNL_RNG_IDX is unexpectedly invalid",
						     rng->range_idx);
						rv = -EINVAL;
						goto out;
					}
				} else {
					_NTs(trace_check_jrnl_rng_blk_skip_err, serjio_pd,
						"Skipping Range @JRNL_RNG_IDX in state @JRANGE_STATUS",
						rng->range_idx, rng->status);
					goto next_rng;
				}
			}
			if (rng->status > JRANGE_ALLOCATED && 
				rng->status != JRANGE_RSVD_DB_ERR && 
				rng->status != JRANGE_QUARANTINED) 
			{
				_NEs(err_check_jrnl_rng_blk_inv_state, serjio_pd,
				     "Range @JRNL_RNG_IDX has invalid state @JRANGE_STATUS",
					rng->range_idx, rng->status);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_rlba != rng_rlba) {
				_NEs(err_check_jrnl_rng_blk_inv_rlba, serjio_pd,
				"Range @JRNL_RNG_IDX has invalid rLBA @RLBA, should be @RLBA",
					rng->range_idx, rng->rng_rlba, rng_rlba);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_nlba != rng_nlba) {
				_NEs(err_check_jrnl_rng_blk_inv_nlba, serjio_pd,
				     "Range @JRNL_RNG_IDX has invalid nLBA @NLBAS should be nLBA @NLBAS",
					rng->range_idx, rng->rng_nlba, rng_nlba);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_rblk != rng_rblk) {
				_NEs(err_check_jrnl_rng_blk_inv_rblk, serjio_pd,
				"Range @JRNL_RNG_IDX has invalid rBLK @BLOCK, should be @BLOCK",
					rng->range_idx, rng->rng_rblk, rng_rblk);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_nblk != rng_nblk) {
				_NEs(err_check_jrnl_rng_blk_inv_nblk, serjio_pd,
				"Range @JRNL_RNG_IDX has invalid number of journal blocks @BLOCKS, should be @BLOCKS",
					rng->range_idx, rng->rng_nblk, rng_nblk);
				rv = -EINVAL;
				goto out;
			}
			exp_n_ents = min_t(unsigned, DISK_LBAS_TO_JOURNAL_ENTS(serjio_pd->di, rng->binje_shift, rng_nlba),
			      NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
			if (rng->n_ents != exp_n_ents) {
				_NEs(err_check_jrnl_rng_blk_inv_nents, serjio_pd,
				     "Range @JRNL_RNG_IDX has invalid num entries @ENTRIES should be @ENTRIES",
					rng->range_idx, rng->n_ents, exp_n_ents);
				rv = -EINVAL;
				goto out;
			}
next_rng:
			rng_rlba += rng_nlba;
			rng_rblk += rng_nblk;
		}
		else {
			if (rng->status == JRANGE_DB_ERR || rng->status == JRANGE_DB_ZERO) {
				_NTs(trace_check_jrnl_rng_blk_skip_err_2, serjio_pd,
				     "Skipping Range @JRNL_RNG_IDX in state @JRANGE_STATUS",
					rng->range_idx, rng->status);
				continue;
			}
			if (rng->status != JRANGE_INVALID) {
				_NEs(err_2_check_jrnl_rng_blk_inv_state, serjio_pd,
				     "Invalid Range @JRNL_RNG_IDX has invalid state @JRANGE_STATUS",
					rng->range_idx, rng->status);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_rlba != NVMEIB_EC_INVALID_JOURNAL_RLBA) {
				_NEs(err_2_check_jrnl_rng_blk_inv_rlba, serjio_pd,
				"Invalid Range @JRNL_RNG_IDX has invalid rLBA @RLBA, should be @RLBA",
					rng->range_idx, rng->rng_rlba, NVMEIB_EC_INVALID_JOURNAL_RLBA);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_nlba != 0) {
				_NEs(err_2_check_jrnl_rng_blk_inv_nlba, serjio_pd,
				     "Invalid Range @JRNL_RNG_IDX has invalid nLBA @NLBAS should be 0",
					rng->range_idx, rng->rng_nlba);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_rblk != NVMEIB_EC_INVALID_JOURNAL_RBLK) {
				_NEs(err_2_check_jrnl_rng_blk_inv_rblk, serjio_pd,
				"Range @JRNL_RNG_IDX has invalid rBLK @BLOCK, should be @BLOCK",
					rng->range_idx, rng->rng_rlba, NVMEIB_EC_INVALID_JOURNAL_RBLK);
				rv = -EINVAL;
				goto out;
			}
			if (rng->rng_nblk != 0) {
				_NEs(err_2_check_jrnl_rng_blk_inv_nblk, serjio_pd,
				"Range @JRNL_RNG_IDX has invalid number of journal blocks @BLOCKS, should be 0",
					rng->range_idx, rng->rng_nblk);
				rv = -EINVAL;
				goto out;
			}
			if (rng->n_ents != 0) {
				_NEs(err_2_check_jrnl_rng_blk_inv_nents, serjio_pd,
				     "Invalid Range @JRNL_RNG_IDX has invalid num entries @ENTRIES should be 0",
					rng->range_idx, rng->n_ents);
				rv = -EINVAL;
				goto out;
			}
		}
	}
out:
	return rv;
}

static int set_jrnl_rng_blk(struct nvmeibs_serjio_disk_private_data *serjio_pd,
			    u32 rng_nlba, u32 rng_nblk,
			    enum nvmeibs_serjio_state srj_state)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *rng;
	int i, j, rv;
	atomic_t io_ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(io_comp);
	unsigned long flags;
	u32 rng_rlba;
	u32 rng_rblk;
	const int n_rng_sched = 8;
	uuid_be new_init_db_uuid;
	
	nvmeibs_serjio_uuid_generate(new_init_db_uuid.b);

	/* All valid ranges are free, update the number of blocks per range */

	/* First we move all the ranges to the invalid state so we can start from a known position */
	for (i = 0, rng = jranges_alloc_tbl->ranges; i < (int)jranges_alloc_tbl->num_ranges; i++, rng++) {
		if (rng->status == JRANGE_FREE || rng->status == JRANGE_QUARANTINED) {
			for (j = 0; j < NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; j++) {
				enum nvmeibs_serjio_jentry_state cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, j);
				enum nvmeibs_serjio_jentry_state exp_jentry_state = j < rng->n_ents ? JENTRY_FREE : JENTRY_INVALID;
				SERJIO_BUG_ON(cur_jentry_state != exp_jentry_state,
						err_set_jrnl_rng_blk_ent_inv_state, serjio_pd,
						"Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX has invalid state @JENTRY_STATE expected @JENTRY_STATE",
						rng->range_idx, 1 << rng->binje_shift, j, cur_jentry_state, exp_jentry_state);
				SET_JENTRY_STATE_IN_BMP(rng->jentry_state_bmp, j, JENTRY_INVALID);
				rng->jentry_md[j].ent_gen_id = NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID;
			}
			for (j = 0; j < MAX_JENTRY_STATE; j++) {
				int exp_cnt = 0;
				if (j == JENTRY_FREE)
					exp_cnt = rng->n_ents;
				else if (j == JENTRY_INVALID)
					exp_cnt = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE - rng->n_ents;
				SERJIO_BUG_ON(rng->jentry_state_cnt[j] != exp_cnt,
						err_set_jrnl_rng_blk_inv_ent_cnt, serjio_pd,
						"Range @JRNL_RNG_IDX has invalid @JENTRY_STATE count @JENTRY_STATE_CNT",
						rng->range_idx, j, rng->jentry_state_cnt[j]);
				rng->jentry_state_cnt[j] = 0;
			}
			rng->jentry_state_cnt[JENTRY_INVALID] = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
			rng->gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
			rng->binje_shift = JRANGE_INVALID_BINJE_SHIFT;
			rng->rng_rlba = NVMEIB_EC_INVALID_JOURNAL_RLBA;
			rng->rng_nlba = 0;
			rng->rng_rblk = NVMEIB_EC_INVALID_JOURNAL_RBLK;
			rng->rng_nblk = 0;
			rng->n_ents = 0;
			hlist_del(&rng->link);
			BUG_ON(jranges_alloc_tbl->num_valid_rngs == 0);
			if (rng->status == JRANGE_FREE) {
				BUG_ON(jranges_alloc_tbl->num_free_rngs == 0);
				jranges_alloc_tbl->num_free_rngs--;
			} else {
				BUG_ON(rng->status != JRANGE_QUARANTINED);
				BUG_ON(jranges_alloc_tbl->num_quarantined_rngs == 0);
				jranges_alloc_tbl->num_quarantined_rngs--;
			}
			jranges_alloc_tbl->num_valid_rngs--;
			rng->status = JRANGE_INVALID;
			hlist_add_head(&rng->link, &jranges_alloc_tbl->invalid_ranges);
		} else if (rng->status == JRANGE_DB_ZERO ||
			rng->status == JRANGE_DB_ERR) {
			/* In either case, switch range to Invalid state
			 *(If there is genuinely an error with the DB entry,
			 * then we will rediscover this when we try to write the updated state to the DB) */
			hlist_del(&rng->link);
			BUG_ON(jranges_alloc_tbl->num_db_err_rngs == 0);
			jranges_alloc_tbl->num_db_err_rngs--;
			if (rng->status == JRANGE_DB_ZERO) {
				BUG_ON(jranges_alloc_tbl->num_db_zero_rngs == 0);
				jranges_alloc_tbl->num_db_zero_rngs--;
			}
			rng->status = JRANGE_INVALID;
			hlist_add_head(&rng->link, &jranges_alloc_tbl->invalid_ranges);
		} else {
			SERJIO_BUG_ON(rng->status != JRANGE_INVALID,
				      bug_set_jrnl_rng_blk_inv_state, serjio_pd,
					"Range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
					rng->range_idx, rng->status);
		}
	}

	for (i = 0, rng_rlba = 0, rng_rblk = 0, rng = jranges_alloc_tbl->ranges;
	     i < (int)jranges_alloc_tbl->num_ranges; i++, rng++)
	{
		if (i == (int)nvmeibs_serjio_init_db_interrupt_range) {
			_NTs(trace_serjio_set_jrnl_rng_blk_interrupt_rng, serjio_pd,
			     "Interrupting Init DB at Range @JRNL_RNG_IDX", i);
			rv = -ECANCELED;
			goto out;
		}
		if (!serjio_state_cmp(serjio_pd, srj_state)) {
			rv = -ECANCELED;
			break;
		}
		if (i % n_rng_sched == 0) {
			cond_resched();
		}
		BUG_ON(rng->status != JRANGE_INVALID);
		rng->init_db_uuid = new_init_db_uuid;
		rng->init_db_uuid_done = NULL_UUID_BE;
		
		/* [NVMESH-5279]: Make ranges 0,1,2 invalid as well as ranges past the end of the partition */
		if ((invalid_jris_exist_on_disk || i >= NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) &&
			rng_rlba < serjio_pd->disk_ranges.journal.len_nlbas)
		{
			/* Range is valid with the updated blocks per range */
			rng->status = i >= NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES ? JRANGE_FREE : JRANGE_QUARANTINED;
			rng->gen_id = 0;
			rng->binje_shift = ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY);
			rng->rng_rlba = rng_rlba;
			rng->rng_nlba = rng_nlba;
			rng->rng_rblk = rng_rblk;
			rng->rng_nblk = rng_nblk;

			/* Increase the rng_rlba, rng_nlba for the next range */
			rng_rlba += rng_nlba;
			rng_rblk += rng_nblk;

			/* Reset the binje which will fix all the entry counts */
			set_rng_binje(rng, rng->binje_shift, JENTRY_INVALID, JENTRY_FREE, NULL);

			/* Zero the journal range */
			if ((rv = zero_journal_range(rng, &io_ctr, &io_comp))) {
				_NEs(error_serjio_set_jrnl_rng_blk_zero_rng, serjio_pd, "zero_journal_entry failed (@RV)", rv);
				/* Lock is needed because the callback from write_jrange_to_serjio_db can also modify this stuff */
				spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
				hlist_del(&rng->link);
				jranges_alloc_tbl->num_db_err_rngs++;
				BUG_ON(jranges_alloc_tbl->num_db_err_rngs > jranges_alloc_tbl->num_ranges);
				rng->status = JRANGE_DB_ERR;
				hlist_add_head(&rng->link, &jranges_alloc_tbl->db_err_ranges);
				spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
				continue;
			}

			/* Lock is needed because the callback from write_jrange_to_serjio_db can also modify this stuff */
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			if (rng->status == JRANGE_FREE)
				jranges_alloc_tbl->num_free_rngs++;
			else if (rng->status == JRANGE_QUARANTINED)
				jranges_alloc_tbl->num_quarantined_rngs++;
			else
				BUG();
			jranges_alloc_tbl->num_valid_rngs++;
			BUG_ON(jranges_alloc_tbl->num_free_rngs > jranges_alloc_tbl->num_ranges);
			BUG_ON(jranges_alloc_tbl->num_valid_rngs > jranges_alloc_tbl->num_ranges);
			hlist_del(&rng->link);
			if (rng->status == JRANGE_FREE)
				hlist_add_head(&rng->link, &jranges_alloc_tbl->free_ranges);
			else if (rng->status == JRANGE_QUARANTINED)
				hlist_add_head(&rng->link, &jranges_alloc_tbl->quarantined_ranges);
			else
				BUG();
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		}
		/* Write the updated range to SERJIO DB */
		if ((rv = write_jrange_to_serjio_db(rng, &io_comp, &io_ctr))) {
			_NWs(warn_serjio_set_jrnl_rng_blk_fail_write_db, serjio_pd,
			     "Failed (@RV) to write SERJIO DB Entry @JRNL_RNG_IDX", rv, rng->range_idx);
			/* Write Failed - Put the jrange in the error list */

			/* Lock is needed because the callback from write_jrange_to_serjio_db can also modify this stuff */
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			hlist_del(&rng->link);
			hlist_add_head(&rng->link, &jranges_alloc_tbl->db_err_ranges);
			if (rng->status == JRANGE_FREE)
				jranges_alloc_tbl->num_free_rngs--;
			else if (rng->status == JRANGE_QUARANTINED)
				jranges_alloc_tbl->num_quarantined_rngs--;
			else
				BUG();
			jranges_alloc_tbl->num_db_err_rngs++;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
			continue;
		}
	}
	if (atomic_dec_return(&io_ctr) > 0)
		wait_for_completion(&io_comp);

	nvmeib_reinit_completion(&io_comp);
	atomic_set(&io_ctr, 1);

	/* Finished writing all ranges to DB, now update the first and last ranges with "init done" */
	for (i = 0; i < (int)jranges_alloc_tbl->num_ranges; i += (jranges_alloc_tbl->num_ranges - 1)) {
		rng = &jranges_alloc_tbl->ranges[i];

		rng->init_db_uuid_done = new_init_db_uuid;

		/* Write the updated range to SERJIO DB */
		if ((rv = write_jrange_to_serjio_db(rng, &io_comp, &io_ctr))) {
			_NWs(warn_serjio_set_jrnl_rng_blk_fail_write_db_2, serjio_pd,
			"Failed (@RV) to write SERJIO DB Entry @JRNL_RNG_IDX", rv, rng->range_idx);
			/* Write Failed - Put the jrange in the error list */
			
			/* Lock is needed because the callback from write_jrange_to_serjio_db can also modify this stuff */
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			hlist_del(&rng->link);
			hlist_add_head(&rng->link, &jranges_alloc_tbl->db_err_ranges);
			jranges_alloc_tbl->num_db_err_rngs++;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
			goto out;
		}
	}
	if (atomic_dec_return(&io_ctr) > 0)
		wait_for_completion(&io_comp);
	rv = 0;

out:
	return rv;
}

static DECLARE_IO_WQ_FN(io_rd_db_fn)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	int i, rv = 0;
	atomic_t ctr;
	DECLARE_COMPLETION_ONSTACK(comp);
	enum nvmeibs_serjio_state init_state = serjio_state_get(serjio_pd);
	enum nvmeibs_serjio_state fini_state_err = SERJIO_ERR_RD_DB;

	NFIN;
	(void)param;
	init_state = serjio_state_get(serjio_pd);
	if (init_state != SERJIO_RD_GPT) {
		if (SERJIO_HALTING(init_state)) {
		_NTs(io_rd_db_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", init_state);
			rv = -ECANCELED;
		} else {
			_NTs(io_rd_db_fn_t2, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", init_state);
			BUG_ON(1);
		}
		goto out;
	}
	BUG_ON(serjio_state_cmp_exch(
		serjio_pd, init_state, SERJIO_RD_DB) != init_state);

	_NTs(io_rd_db_fn_t3, serjio_pd, "Reading SERJIO DB");

	/* Read the SERJIO DB */
	atomic_set(&ctr, 1);
	for (i = 0; i < (int)jranges_alloc_tbl->num_ranges; i++) {
		if (!serjio_state_cmp(serjio_pd, SERJIO_RD_DB)) {
			rv = -ECANCELED;
			break;
		}
		atomic_inc(&ctr);
		if ((rv = read_serjio_db_jrange_entry(serjio_pd, i, &ctr, &comp))) {
			_NEs(error_serjio_io_rd_db_fn, serjio_pd, "Error (@RV) reading SERJIO DB index @INDEX", rv, i);
			atomic_dec(&ctr);
			break;
		}
	}
	if (atomic_dec_return(&ctr) > 0)
		wait_for_completion(&comp);

	if (rv)
		goto out;

	if (jranges_alloc_tbl->num_db_zero_rngs == jranges_alloc_tbl->num_ranges) {
		_NTs(trace_2_serjio_io_rd_db_fn, serjio_pd, "SERJIO DB has been zeroed. Initializing Journal");
		/* Schedule the next work to init the journal */
		rv = run_on_io_wq(serjio_pd, io_init_jrnl_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_INIT_JRNL);
		if (rv == -EINPROGRESS)
			rv = 0;
		goto out;
	} else {
		struct jrange_entry *first_jrng = &jranges_alloc_tbl->ranges[0];
		struct jrange_entry *last_jrng = &jranges_alloc_tbl->ranges[jranges_alloc_tbl->num_ranges - 1];
		struct jrange_entry *jrng = first_jrng->status <= JRANGE_STATUS_DB_MAX ? first_jrng : last_jrng;
		bool found_valid_rng = false;

		SERJIO_BUG_ON(jrng->status == JRANGE_UNKNOWN, bug_serjio_io_rd_db_fn_rng_inv_state, serjio_pd,
			      "Range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
				jrng->range_idx, jrng->status);
		
		if (jrng->status > JRANGE_STATUS_DB_MAX) {
			_NEs(err_serjio_io_rd_db_fn_both_first_and_last_rng_err, serjio_pd,
			     "SERJIO does not support recovery from DB errors in both first range and last range @JRNL_RNG_IDX (state @JRANGE_STATUS)",
			     jrng->range_idx, jrng->status);
			rv = -ENOTRECOVERABLE;
			goto out;
		}

		if (nvmeib_uuid_cmp(jrng->init_db_uuid,  jrng->init_db_uuid_done) != 0) {
			/* Init DB was interrupted, schedule work to re-init the journal */
			_NWs(trace_serjio_io_rd_db_fn_rng_init_db_interrupt, serjio_pd,
			     "Detected Interruption of Journal Initialization. Resuming");
			/* Schedule the next work to init the journal */
			rv = run_on_io_wq(serjio_pd, io_init_jrnl_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_INIT_JRNL);
			if (rv == -EINPROGRESS)
				rv = 0;
			goto out;
		}

		/* Find the first valid range to check the nlba */
		for (jrng = first_jrng; jrng <= last_jrng; jrng++) {
			if (jrng->status >= JRANGE_STATUS_DB_VALID_MIN &&
				jrng->status <= JRANGE_STATUS_DB_VALID_MAX)
			{
				found_valid_rng = true;
				break;
			}
		}

		if (!found_valid_rng) {
			_NEs(err_serjio_io_rd_db_fn_no_valid_ranges, serjio_pd,
			     "No valid ranges found. Unrecoverable");
			rv = -ENOTRECOVERABLE;
			goto out;
		}

		if (jrng->rng_nlba != serjio_pd->rng_nlba) {
			if (jranges_alloc_tbl->num_free_rngs + jranges_alloc_tbl->num_quarantined_rngs != jranges_alloc_tbl->num_valid_rngs) {
				_NEs(trace_serjio_io_rd_db_fn_rng_nlba_chng_invalid, serjio_pd,
				     "Journal Blocks per range changed from @N_BLOCKS to @N_BLOCKS while Journal is dirty. Aborting",
					DISK_LBAS_TO_JOURNAL_BLKS(serjio_pd->di, jrng->rng_nlba),
					nvmeibs_jrange_num_blocks);
				rv = -EINVAL;
				goto out;
			}
			/* First, verify there are no inconsistencies between the ranges */
			rv = check_jrnl_rng_blk(serjio_pd, jrng->rng_nlba, jrng->rng_nblk);
			if (rv)
				goto out;
			/* Set the number of blocks per journal range */
			rv = set_jrnl_rng_blk(serjio_pd, serjio_pd->rng_nlba, serjio_pd->rng_nblk, SERJIO_RD_DB);
			if (rv)
				goto out;
		} else {
			/* Verify there are no inconsistencies between the ranges */
			rv = check_jrnl_rng_blk(serjio_pd, serjio_pd->rng_nlba, serjio_pd->rng_nblk);
			if (rv)
				goto out;
		}
	}
	/* Schedule the next work to read the journal */
	rv = run_on_io_wq(serjio_pd, io_rd_jrnl_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_JRNL);
	if (rv == -EINPROGRESS)
		rv = 0;

out:
	if (rv && rv != -ECANCELED) {
		/* Failed to read the SERJIO DB - Put SERJIO in Error State */
		serjio_go_to_err_state(serjio_pd, fini_state_err);
	}
	NFOUT;
	return rv;
}

static int rd_jrange(struct nvmeibs_serjio_disk_private_data *serjio_pd,
				   nvme_callback_t read_cb, void *cb_param, struct jrange_entry *jrng,
				   unsigned long read_ent_state_mask, unsigned long *read_ent_bmp,
				   atomic_t *ctr, struct completion *comp)
{
	enum nvmeibs_serjio_jentry_state cur_jentry_state;
	unsigned entry;
	int rv = 0;
	atomic_t int_ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(int_comp);
	unsigned long flags;
	DECLARE_BITMAP(rd_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) = {0};
	int state;
	int state_mask_cnt = 0;

	NFIN;
	if (!comp) {
		comp = &int_comp;
		ctr = &int_ctr;
	}
	if (jrng->status == JRANGE_INVALID) {
		_NEs(err_rd_jrange_invalid, serjio_pd,
		     "Cannot read invalid range @JRNL_RNG_IDX",
			jrng->range_idx);
		rv = -EINVAL;
		goto out;
	}
	SERJIO_BUG_ON(jrng->n_ents == 0,
			bug_rd_jrange_no_ents, serjio_pd,
			"Range @JRNL_RNG_IDX has no entries",
			jrng->range_idx);
	SERJIO_BUG_ON(jrng->rng_nblk == 0,
			bug_rd_jrange_no_blks, serjio_pd,
			"Range @JRNL_RNG_IDX has no journal blocks",
			jrng->rng_nblk);
	spin_lock_irqsave(&jrng->lock, flags);
	for (entry = 0; entry < jrng->n_ents; entry++) {
		cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, entry);
		if ((test_bit(cur_jentry_state, &read_ent_state_mask)) &&
				(!read_ent_bmp || test_bit(entry, read_ent_bmp)))
			set_bit(entry, rd_ents_bmp);
	}
	/* [NVMESH-7216]: Check for mismatch between rd_ents_bmp and state counters */
	if (!read_ent_bmp) {
		for_each_set_bit(state, &read_ent_state_mask, MAX_JENTRY_STATE) {
			state_mask_cnt += jrng->jentry_state_cnt[state];
		}
		if (bitmap_empty(rd_ents_bmp, jrng->n_ents) && state_mask_cnt > 0)
		{
			_NEs_dmesg(warn_serjio_rd_jrange_empty_bmp_with_unsynced, serjio_pd,
				"Range @JRNL_RNG_IDX: Empty rd_ents_bmp when it should not be empty- "
				"mask=@JENTRY_STATE_MASK entries=@JENTRY_STATE_CNT cnt[UNKNOWN]=@JENTRY_STATE_CNT cnt[SYNCED]=@JENTRY_STATE_CNT "
				"cnt[FREE]=@JENTRY_STATE_CNT cnt[ABND]=@JENTRY_STATE_CNT cnt[TAKEN]=@JENTRY_STATE_CNT "
				"cnt[WAIT_RET]=@JENTRY_STATE_CNT cnt[IO_ERR]=@JENTRY_STATE_CNT cnt[INVALID]=@JENTRY_STATE_CNT",
				jrng->range_idx, (unsigned)read_ent_state_mask, jrng->n_ents,
				jrng->jentry_state_cnt[JENTRY_UNKNOWN],
				jrng->jentry_state_cnt[JENTRY_SYNCED],
				jrng->jentry_state_cnt[JENTRY_FREE],
				jrng->jentry_state_cnt[JENTRY_ABND],
				jrng->jentry_state_cnt[JENTRY_TAKEN],
				jrng->jentry_state_cnt[JENTRY_WAIT_RET],
				jrng->jentry_state_cnt[JENTRY_IO_ERR],
				jrng->jentry_state_cnt[JENTRY_INVALID]);
				BUG_NON_PRODUCTION(7216);
		}
	}
	spin_unlock_irqrestore(&jrng->lock, flags);
	_NTs(trace_serjio_rd_jrange_ents, serjio_pd,
		 "Reading Range @JRNL_RNG_IDX Entries: "
		NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE,
		jrng->range_idx, rd_ents_bmp);
	for_each_set_bit(entry, rd_ents_bmp, jrng->n_ents) {
		if ((rv = read_jrnl_entry(jrng, entry, ctr, comp, read_cb, cb_param))) {
			_NEs(error_serjio_rd_jrange, serjio_pd, "Error (@RV) reading Journal Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX",
				rv, jrng->range_idx, entry);
			goto out;
		}
	}
out:
	if (comp == &int_comp && atomic_dec_return(ctr) > 0)
		wait_for_completion(comp);
	NFOUT;
	return rv;
}

static int rd_jrnl(struct nvmeibs_serjio_disk_private_data *serjio_pd,
				   nvme_callback_t read_cb, void *cb_param,
				   enum nvmeibs_serjio_state check_state, bool read_free_rng,
				   unsigned long read_ent_state_mask)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	int i, rv = 0;
	atomic_t read_ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(read_comp);
	unsigned long start_jiffies = jiffies;

	for (i = 0; i < (int)jranges_alloc_tbl->num_ranges; i++) {
		struct jrange_entry *jrange_entry = &jranges_alloc_tbl->ranges[i];
		if (!serjio_state_cmp(serjio_pd, check_state)) {
			rv = -ECANCELED;
			goto wait_comp;
		}
		if (jrange_entry->status > JRANGE_ALLOCATED && jrange_entry->status != JRANGE_QUARANTINED) {
			_NTs(trace_serjio_rd_jrnl_skip_range_inv_state, serjio_pd,
			     "Skipping range @JRNL_RNG_IDX in state @JRANGE_STATUS", 
				jrange_entry->range_idx, jrange_entry->status);
			continue;
		}
		SERJIO_BUG_ON(jrange_entry->status == JRANGE_UNKNOWN,
			      bug_rd_jrnl_valid_rng_inv_status, serjio_pd,
				"Range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
				jrange_entry->status, jrange_entry->range_idx);
		SERJIO_BUG_ON(jrange_entry->rng_nblk == 0,
			      bug_rd_jrnl_valid_rng_no_blk, serjio_pd,
				"Range @JRNL_RNG_IDX in state @JRANGE_STATUS has no blocks",
				jrange_entry->status, jrange_entry->range_idx);
		if (read_free_rng || (jrange_entry->status != JRANGE_FREE && jrange_entry->status != JRANGE_QUARANTINED)) {
			if ((rv = rd_jrange(serjio_pd, read_cb, cb_param, jrange_entry,
				read_ent_state_mask, NULL, &read_ctr, &read_comp))) {
				goto wait_comp;
			}
		}
	}
wait_comp:
	if (atomic_dec_return(&read_ctr) > 0)
		wait_for_completion(&read_comp);

	_NTs(trace_serjio_rd_jrnl, serjio_pd, "Took @DIFF_JIFFIES jiffies (@DIFF_JIFFIES ms) to read journal",
		jiffies - start_jiffies, (jiffies - start_jiffies) * 1000 / HZ);
	return rv;
}

/* Return Journal Range to the Free State -
 * Must be in the RESERVED, DB_ZERO or DB_ERR states with all entries freed */
static int free_jrnl_rng(struct jrange_entry *rng, struct completion *comp, atomic_t *ctr)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = rng->serjio_pd;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	unsigned long flags;
	unsigned ent_idx;
	int rv;
	DECLARE_COMPLETION_ONSTACK(int_comp);
	atomic_t int_ctr = ATOMIC_INIT(1);
	DECLARE_BITMAP(set_binje_zero_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) = {};

	NFIN;
	/* First remove from the current list */
	spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
	hlist_del(&rng->link);
	switch (rng->status) {
	case JRANGE_RESERVED:
		jranges_alloc_tbl->num_reserved_ranges--;
		SERJIO_BUG_ON(rng->jentry_state_cnt[JENTRY_FREE] < rng->n_ents,
			free_jrnl_rng_e1, serjio_pd, "[JENTRY_FREE]=@UINT NUM_ENTRIES=@INT N=@BINJE",
			rng->jentry_state_cnt[JENTRY_FREE], rng->n_ents, 1 << rng->binje_shift);
		break;
	case JRANGE_DB_ZERO:
		jranges_alloc_tbl->num_db_zero_rngs--;
		FALLTHRU;
	case JRANGE_DB_ERR:
		jranges_alloc_tbl->num_db_err_rngs--;
		break;
	default:
		BUG_ON(1);
	}
	spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);

	/* Then clear the range state */
	spin_lock_irqsave(&rng->lock, flags);
	if (rng->range_idx < NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) {
		/* [NVMESH-5279]: Invalid JRI was allocated to client and is now free. 
		 *	==> Add to invalid list */
		_NTs(trace_serjio_free_jrnl_rng_fail_inv_jri, serjio_pd,
		     "Range @JRNL_RNG_IDX is invalid for client "
		     "(< NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES @JRNL_RNG_IDX). Quarantining",
		     rng->range_idx, NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES);
		rng->status = JRANGE_QUARANTINED;
	} else {
		rng->status = JRANGE_FREE;
	}
	rng->client_id = 0;
	rng->client_uuid = NULL_UUID_BE;
	rng->client_host[0] = 0;
	memset(&rng->reserved, 0, sizeof(rng->reserved));
	memset(&rng->last_allocated, 0, sizeof(rng->last_allocated));
	memset(&rng->last_returned, 0, sizeof(rng->last_returned));
	rng->gen_id++;
	rng->ent_gen_id_wrapped = false;

	for (ent_idx = 0; ent_idx < rng->n_ents; ent_idx++) {
		BUG_ON(GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, ent_idx) != JENTRY_FREE);
		rng->jentry_md[ent_idx].ent_gen_id = nvmeib_jrnl_ent_gen_id_min;
	}
	rng->returned_jif = 0;

	/* Set Range N to Default */
	set_rng_binje(rng, ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY), JENTRY_FREE, JENTRY_FREE, set_binje_zero_ents_bmp);

	spin_unlock_irqrestore(&rng->lock, flags);

	/* Add range to free list
	 * [NVMESH-4331]: Do this before the DB write. If the write fails, the callback will move it to the db_err_ranges list
	*/
	spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
	if (rng->status == JRANGE_QUARANTINED) {
		hlist_add_head(&rng->link, &jranges_alloc_tbl->quarantined_ranges);
		jranges_alloc_tbl->num_quarantined_rngs++;
	} else {
		hlist_add_head(&rng->link, &jranges_alloc_tbl->free_ranges);
		jranges_alloc_tbl->num_free_rngs++;
	}
	spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);

	/* Now write to the disk */
	if (!comp) {
		comp = &int_comp;
		ctr = &int_ctr;
	}
	for_each_set_bit(ent_idx, set_binje_zero_ents_bmp, rng->n_ents) {
		if ((rv = zero_journal_entry(serjio_pd, rng, ent_idx, ctr, comp, NULL, NULL,
			"%s - rng: %u binje: %u entry: %d needs zero due to binje change\n",
			__func__, rng->range_idx, 1 << rng->binje_shift, ent_idx)) < 0)
		{
			_NWs(warn_serjio_free_jrnl_rng_fail_zero_ent, serjio_pd,
			     "Failed (@RV) to Zero Range @JRNL_RNG_IDX Entry @JENT_IDX",
			     rv, rng->range_idx, ent_idx);
			/* Write Failed - Put the jrange in the error list */
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			hlist_add_head(&rng->link, &jranges_alloc_tbl->db_err_ranges);
			jranges_alloc_tbl->num_db_err_rngs++;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
			goto out;
		}
	}
	if ((rv = write_jrange_to_serjio_db(rng, comp, ctr))) {
		_NWs(warn_serjio_free_jrnl_rng, serjio_pd, "Failed (@RV) to write SERJIO DB Entry @JRNL_RNG_IDX", rv, rng->range_idx);
		/* Write Failed - Put the jrange in the error list */
		spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
		hlist_del(&rng->link);
		if (rng->range_idx >= NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) 
			jranges_alloc_tbl->num_free_rngs--;
		hlist_add_head(&rng->link, &jranges_alloc_tbl->db_err_ranges);
		jranges_alloc_tbl->num_db_err_rngs++;
		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		goto out;
	}
	if (comp == &int_comp && atomic_dec_return(&int_ctr) > 0)
		wait_for_completion(&int_comp);

	rv = 0;
out:
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_rd_jrnl_fn)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	int i, rv = 0;
	atomic_t io_ctr;
	DECLARE_COMPLETION_ONSTACK(io_comp);
	enum nvmeibs_serjio_state init_state;
	enum nvmeibs_serjio_state fini_state_err = SERJIO_ERR_RD_JRNL;
	unsigned inv_sblk;
	unsigned j;

	NFIN;
	(void)param;
	init_state = serjio_state_get(serjio_pd);
	if (init_state != SERJIO_RD_DB) {
		if (SERJIO_HALTING(init_state)) {
		_NTs(io_rd_jrnl_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", init_state);
			rv = -ECANCELED;
		} else {
			_NTs(io_rd_jrnl_fn_t2, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", init_state);
			BUG_ON(1);
		}
		goto out;
	}
	BUG_ON(serjio_state_cmp_exch(
		serjio_pd, init_state, SERJIO_RD_JRNL) != init_state);

	_NTs(io_rd_jrnl_fn_t3, serjio_pd, "Reading Journal");

	/* Read the Journal to populate the JMDC */
	rv = rd_jrnl(serjio_pd, read_jmdc_entry_cb, NULL, SERJIO_RD_JRNL, true, JENTRY_ALL_MASK);
	if (rv)
		goto out;

	atomic_set(&io_ctr, 1);
	/* Loop over the JMDC to rebuild the entry states.
	 * If all entries are free, then the range can be freed */
	for (i = 0; i < (int)jranges_alloc_tbl->num_ranges; i++) {
		struct jrange_entry *jrange_entry = &jranges_alloc_tbl->ranges[i];
		if (jrange_entry->status > JRANGE_ALLOCATED && jrange_entry->status != JRANGE_QUARANTINED) {
			_NTs(trace_serjio_io_rd_jrnl_fn_inv_rng_state, serjio_pd,
				"Skipping Range @JRNL_RNG_IDX in state @JRANGE_STATUS",
				jrange_entry->range_idx, jrange_entry->status);
			continue;
		}
		if (jrange_entry->status != JRANGE_FREE &&
			jrange_entry->status != JRANGE_QUARANTINED) {
			if (jrange_entry->status == JRANGE_ALLOCATED) {
				_NTs(trace_2_serjio_io_rd_jrnl_fn, serjio_pd, "Journal range @JRNL_RNG_IDX (client @CLIENT_UUID, @CLIENT_HOST) was not cleanly returned",
					jrange_entry->range_idx, &jrange_entry->client_uuid, jrange_entry->client_host);
				jrange_entry->status = JRANGE_RESERVED;
			}
		}
		if (jrange_entry->status == JRANGE_RESERVED) {
			if (jrange_entry->jentry_state_cnt[JENTRY_FREE] == jrange_entry->n_ents) {
				_NDs(trace_3_serjio_io_rd_jrnl_fn, serjio_pd, "Journal range @JRNL_RNG_IDX clear. Clearing client @CLIENT_UUID from Serjio DB",
					jrange_entry->range_idx, &jrange_entry->client_uuid);
				if (!serjio_state_cmp(serjio_pd, SERJIO_RD_JRNL)) {
					rv = -ECANCELED;
					break;
				}
				/* All entires in the journal range are clear, we can free the journal range */
				free_jrnl_rng(jrange_entry, &io_comp, &io_ctr);
			} else if (jrange_entry->ent_gen_id_wrapped) {
				jrange_entry->gen_id++;
				jrange_entry->ent_gen_id_wrapped = false;
				if ((rv = write_jrange_to_serjio_db(jrange_entry, &io_comp, &io_ctr)) < 0) {
					_NEs(io_rd_jrnl_fn_e1, serjio_pd, "write_jrange_to_serjio_db failed (@INT) for range @UINT",
						rv, jrange_entry->range_idx);
				}
			}
		}
		/* Set all invalid jblocks to nvmeib_jmd_invalid_special_val
		 * (invalid due to max entries * binje < range jblocks) */
		inv_sblk = (jrange_entry->n_ents << jrange_entry->binje_shift);
		if (jrange_entry->rng_nblk > inv_sblk) {
			for (j = inv_sblk; j < jrange_entry->rng_nblk; j += (1 << jrange_entry->binje_shift))
				nvmeib_shared_set_jentry_md_invalid_special(get_jmdc_block(jrange_entry, j), (1 << jrange_entry->binje_shift));
		}
		sync_jmdc_range_to_all_nics(serjio_pd, i);
	}
	if (atomic_dec_return(&io_ctr) > 0)
		wait_for_completion(&io_comp);

	if (rv)
		goto out;

	/* Go to ready state */
	rv = run_on_io_wq(serjio_pd, io_ready_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_READY);

out:
	if (rv && rv != -ECANCELED) {
		/* Failed to read the Journal - Put SERJIO in Error State */
		serjio_go_to_err_state(serjio_pd, fini_state_err);
	}
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_ready_fn)
{
	int rv = 0;
	enum nvmeibs_serjio_state init_state = serjio_state_get(serjio_pd);
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;

	NFIN;
	(void)param;
	if (init_state != SERJIO_INIT_JRNL && init_state != SERJIO_RD_JRNL) {
		if (SERJIO_HALTING(init_state)) {
		_NTs(io_ready_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", init_state);
			rv = -ECANCELED;
		} else {
			_NTs(io_ready_fn_t2, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", init_state);
			rv = -EINVAL;
			BUG_ON(1);
		}
		goto out;
	}
	BUG_ON(serjio_state_cmp_exch(
		serjio_pd, init_state, SERJIO_READY) != init_state);

	_NTs(error_serjio_io_ready_fn, serjio_pd, "Disk @DISK_ID_STR SERJIO Ready. @NUM_RANGES Journal Ranges (@NUM_RANGES Valid Ranges, @NUM_RESERVED_RANGES Reserved, @NUM_FREE_RNGS Free, @NUM_DB_ERR_RNGS Error, @NUM_DB_ZERO_RNGS Zero)",
		nvmeibs_disk_info_get_disk_id(serjio_pd->di), jranges_alloc_tbl->num_ranges,
		jranges_alloc_tbl->num_valid_rngs,
		jranges_alloc_tbl->num_reserved_ranges, jranges_alloc_tbl->num_free_rngs,
		jranges_alloc_tbl->num_db_err_rngs, jranges_alloc_tbl->num_db_zero_rngs);

	nvmeibs_toma_report_event_serjio_state_change(
		nvmeibs_disk_info_get_disk_id(serjio_pd->di),
		nvmeibs_nvme_get_vendor(serjio_pd->di),
		nvmeibs_nvme_get_model(serjio_pd->di->dev), NVMEIBS_SERJIO_STATUS_READY);

	/* Check if want to launch a JGC for any reserved ranges */
	chk_launch_new_jgc(serjio_pd, true);

out:
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_init_jrnl_fn)
{
	int rv = 0;
	enum nvmeibs_serjio_state init_state = serjio_state_get(serjio_pd);
	enum nvmeibs_serjio_state fini_state_err = SERJIO_ERR_WR_DB;

	NFIN;
	(void)param;
	if (init_state != SERJIO_RD_DB && init_state != SERJIO_GPT_INIT) {
		if (SERJIO_HALTING(init_state)) {
		_NTs(io_init_jrnl_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", init_state);
			rv = -ECANCELED;
		} else {
			_NTs(io_init_jrnl_fn_t2, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", init_state);
			BUG_ON(1);
		}
		goto out;
	}
	BUG_ON(serjio_state_cmp_exch(
		serjio_pd, init_state, SERJIO_INIT_JRNL) != init_state);

	_NTs(io_init_jrnl_fn_t3, serjio_pd, "Initialising SERJIO DB");

	/* SERJIO DB has been zeroed - Zero all journal entries and Initialise all DB Entries */
	rv = set_jrnl_rng_blk(serjio_pd, serjio_pd->rng_nlba, serjio_pd->rng_nblk, SERJIO_INIT_JRNL);
	if (rv)
		goto out;

	/* Go to ready state */
	rv = run_on_io_wq(serjio_pd, io_ready_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_READY);

out:
	if (rv && rv != -ECANCELED) {
		/* Failed to init the Journal - Put SERJIO in Error State */
		serjio_go_to_err_state(serjio_pd, fini_state_err);
	}
	NFOUT;
	return rv;
}

static void chk_launch_new_jgc(struct nvmeibs_serjio_disk_private_data *serjio_pd, bool on_boot)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *rng;
	unsigned long flags;
	int rv;
	unsigned i, entry;
	u64 cur_jif = jiffies;
	unsigned long *seg_bmp;
	enum nvmeibs_serjio_jentry_state jentry_state;
	struct seg_tree_entry *seg_entry;
	unsigned jgc_avail_ent_low_wm_mult = nvmeibs_serjio_jgc_avail_ent_low_wm_mult;
	unsigned jgc_avail_ent_low_wm_div = nvmeibs_serjio_jgc_avail_ent_low_wm_div;

	seg_bmp = kzalloc(BITS_TO_LONGS(serjio_pd->n_gpt_ents) * sizeof(long), GFP_KERNEL);
	if (!seg_bmp) {
		_NE(error_serjio_chk_launch_new_jgc, "Memory Allocation Error");
		goto out;
	}
	for (i = 0, rng = jranges_alloc_tbl->ranges; i < jranges_alloc_tbl->num_ranges; i++, rng++) {
		spin_lock_irqsave(&rng->lock, flags);
		/* Only launch JGC for Allocated/Reserved Ranges */
		if (rng->status != JRANGE_ALLOCATED && rng->status != JRANGE_RESERVED) {
			goto next_rng;
		}
		/* Only launch JGC if there are UNKNOWN or SYNCED entries */
		if (rng->jentry_state_cnt[JENTRY_UNKNOWN] == 0 &&
			rng->jentry_state_cnt[JENTRY_SYNCED] == 0 &&
			rng->jentry_state_cnt[JENTRY_ABND] == 0) {
			goto next_rng;
		}
		/* For reserved range, check reconnect timeout and for allocated range check watermark */
		if (rng->status == JRANGE_RESERVED &&
			(!on_boot && cur_jif < rng->returned_jif + NVMEIB_JGC_RNG_RECONNECT_TIMEOUT)) {
			goto next_rng;
		}
		if (rng->status == JRANGE_ALLOCATED && rng->jentry_state_cnt[JENTRY_TAKEN] >=
				((rng->n_ents * jgc_avail_ent_low_wm_mult) /
					jgc_avail_ent_low_wm_div)) {
			goto next_rng;
		}
		for (entry = 0; entry < rng->n_ents; entry++) {
			union jblock_md *jmdc_ent = get_jmdc_entry(rng, entry);
			const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_ent);
			jentry_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, entry);
			if (jentry_state == JENTRY_UNKNOWN || jentry_state == JENTRY_SYNCED || jentry_state == JENTRY_ABND) {
				if ((seg_entry = seg_tree_iter_first(&serjio_pd->jrnl_seg_rb_root,
					j2d, j2d))) {
					set_bit(seg_entry->gpt_idx, seg_bmp);
				}
			}
		}
next_rng:
		spin_unlock_irqrestore(&rng->lock, flags);
	}
	for_each_set_bit(i, seg_bmp, serjio_pd->n_gpt_ents) {
		if (!(seg_entry = radix_tree_lookup(&serjio_pd->jrnl_seg_radix_root, i))) {
			BUG();
		}
		if (seg_entry->jgc_launched_jif) {
			/* JGC already launched => Check timeout */
			if (cur_jif < seg_entry->jgc_launched_jif + NVMEIB_JGC_NO_RESP_TIMEOUT)
				continue;
			_NTs(trace_serjio_chk_launch_new_jgc_seg_timeout, serjio_pd,
				"JGC Timeout on Segment @JGC_SEG_ID_STR\n", seg_entry->seg_uuid_str);
			seg_entry->jgc_launched_jif = 0;
		}
		if (!(rv = nvmeibs_serjio_request_jgc(serjio_pd->di, seg_entry->seg_uuid_str))) {
			_NTs(trace_serjio_chk_launch_new_jgc, serjio_pd, "Launched JGC for Segment @JGC_SEG_ID_STR on Disk @DISK_ID_STR (Vendor @VENDOR)",
				seg_entry->seg_uuid_str, nvmeibs_disk_info_get_disk_id(serjio_pd->di),
				nvmeibs_nvme_get_vendor(serjio_pd->di));
			seg_entry->jgc_launched_jif = cur_jif;
		} else {
			_NTs(trace_1_serjio_chk_launch_new_jgc, serjio_pd, "Failed to launch JGC for Segment @JGC_SEG_ID_STR on Disk @DISK_ID_STR (Vendor @VENDOR)",
				seg_entry->seg_uuid_str, nvmeibs_disk_info_get_disk_id(serjio_pd->di),
				nvmeibs_nvme_get_vendor(serjio_pd->di));
		}
	}
	if (!IS_SERJIO_DYING(serjio_pd) && !timer_pending(&serjio_pd->jgc_timer))
		mod_timer(&serjio_pd->jgc_timer, jiffies + NVMEIB_JGC_NO_RESP_TIMEOUT);
out:
	kfree(seg_bmp);
}

static void set_rng_binje(struct jrange_entry *jrng, int new_binje_shift,
			  enum nvmeibs_serjio_jentry_state old_valid_ent_state,
			  enum nvmeibs_serjio_jentry_state new_valid_ent_state,
			  unsigned long *ents_to_zero_bmp)

{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	unsigned i;
	u8 old_binje_shift;
	u16 old_n_ents;
	unsigned old_max_jblk, new_max_jblk;

	_NTs(trace_set_rng_binje, serjio_pd,
			"Range @JRNL_RNG_IDX N @BINJE -> N @BINJE",
			jrng->range_idx, 1 << jrng->binje_shift, 1 << new_binje_shift);

	for (i = 0; i < MAX_JENTRY_STATE; i++)
	{
		int expected_cnt = 0;
		if (i == JENTRY_INVALID)
			expected_cnt = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE - jrng->n_ents;
		else if (i == old_valid_ent_state)
			expected_cnt = jrng->n_ents;
		SERJIO_BUG_ON(jrng->jentry_state_cnt[i] != expected_cnt,
						err_set_rng_binje_rng_inv_ent_cnt, serjio_pd,
						"@JRNL_RNG_IDX has invalid @JENTRY_STATE count @JENTRY_STATE_CNT",
						jrng->range_idx, i, jrng->jentry_state_cnt[i]);
	}

	old_binje_shift = jrng->binje_shift;
	old_n_ents = jrng->n_ents;
	old_max_jblk = old_n_ents << old_binje_shift;
	jrng->binje_shift = new_binje_shift;
	jrng->n_ents = min_t(unsigned, DISK_LBAS_TO_JOURNAL_ENTS(serjio_pd->di, new_binje_shift, jrng->rng_nlba),
			     NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);

	/* Verified old N and that range is clean, set new N */
	/* Check new entry states are all free and jmdc value is unused */
	for (i = 0; i < jrng->n_ents; i++) {
		enum nvmeibs_serjio_jentry_state exp_state = i < old_n_ents ? old_valid_ent_state : JENTRY_INVALID;
		SERJIO_BUG_ON(GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, i) != exp_state,
					  err_set_rng_binje_rng_ent_state_not_free, serjio_pd,
		"Range @JRNL_RNG_IDX N @BINJE (New N @BINJE) Entry @JENT_IDX has invalid state @JENTRY_STATE expected @JENTRY_STATE",
					  jrng->range_idx, 1 << old_binje_shift, 1 << new_binje_shift, i,
					  GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, i), exp_state);
		if (ents_to_zero_bmp && new_valid_ent_state == JENTRY_FREE) {
			/* Because of the change in binje, some previous out of bounds jblocks,
			 * might need zeroing, check for those */
			union jblock_md *jmdc_ent = get_jmdc_entry(jrng, i);
			if (!nvmeib_jentry_md_is_unused_slow(jmdc_ent, 1 << jrng->binje_shift)) {
				unsigned ent_sblk = i << jrng->binje_shift;
				unsigned ent_eblk = ent_sblk + (1 << jrng->binje_shift);
				set_bit(i, ents_to_zero_bmp);
				SERJIO_BUG_ON(ent_eblk < old_max_jblk,
						err_set_rng_binje_rng_ent_jmdc_not_unused, serjio_pd,
						"Range @JRNL_RNG_IDX, Entry @JENT_IDX, Blocks [@BLOCK, @BLOCK) needs zeroing and should be already (jmdc @RAW)",
						jrng->range_idx, i, ent_sblk, ent_eblk, *(u64 *)jmdc_ent);
				_NTs(trace_set_rng_binje_need_zero, serjio_pd,
				     "Range @JRNL_RNG_IDX, Entry @JENT_IDX, Blocks [@BLOCK, @BLOCK) needs to be zeroed due to new BINJE @BINJE",
					jrng->range_idx, i, ent_sblk, ent_eblk, 1 << jrng->binje_shift);
			}
		}
		SET_JENTRY_STATE_IN_BMP(jrng->jentry_state_bmp, i, new_valid_ent_state);
		jrng->jentry_md[i].ent_gen_id = nvmeib_jrnl_ent_gen_id_min;
	}
	/* Set remaining entry states (for N > 1) to be invalid */
	for (; i < NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; i++) {
		SET_JENTRY_STATE_IN_BMP(jrng->jentry_state_bmp, i, JENTRY_INVALID);
		jrng->jentry_md[i].ent_gen_id = NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID;
	}
	/* Set all invalid jblocks to nvmeib_jmd_invalid_special_val
	* (invalid due to max entries * binje < range jblocks) */
	new_max_jblk = jrng->n_ents << jrng->binje_shift;
	if (jrng->rng_nblk > new_max_jblk) {
		unsigned j;
		for (j = new_max_jblk; j < jrng->rng_nblk; j += (1 << jrng->binje_shift))
			nvmeib_shared_set_jentry_md_invalid_special(get_jmdc_block(jrng, j), (1 << jrng->binje_shift));
	}

	/* Set counts that changed (the rest have already been checked to be 0) */
	jrng->jentry_state_cnt[old_valid_ent_state] = 0;
	jrng->jentry_state_cnt[new_valid_ent_state] = jrng->n_ents;
	jrng->jentry_state_cnt[JENTRY_INVALID] = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE - jrng->n_ents;
}

/* Helper function to allocate a range to a client */
static void alloc_rng_to_client(struct nvmeibs_serjio_disk_private_data *serjio_pd,
	struct jrange_entry *jrange, u32 client_id, uuid_be client_uuid,
	const char *client_host, int binje_req_shift,
	unsigned long *binje_chng_zero_ents)
{
	unsigned i;
	unsigned long flags;

	(void)serjio_pd;
	spin_lock_irqsave(&jrange->lock, flags);
	jrange->status = JRANGE_ALLOCATED;
	jrange->client_id = client_id;
	strlcpy(jrange->client_host, client_host, sizeof(jrange->client_host));
	jrange->client_uuid = client_uuid;
	getnstimeofday(&jrange->reserved);
	getnstimeofday(&jrange->last_allocated);
	jrange->gen_id++;
	jrange->ent_gen_id_wrapped = false;

	set_rng_binje(jrange, binje_req_shift, JENTRY_FREE, JENTRY_FREE, binje_chng_zero_ents);

	for (i = 0; i < jrange->n_ents; i++) {
		BUG_ON(JENTRY_STATE_CHNG(jrange, i, JENTRY_TAKEN, true, JENTRY_STATE_CHNG_REASON_ALLOC_FREE_RNG) != JENTRY_FREE);
	}

	spin_unlock_irqrestore(&jrange->lock, flags);
}

/* Helper function to update allocated range */
static int upd_alloc_rng(struct jrange_entry *jrange, u32 client_id, unsigned binje_req_shift,
			const struct nvmeib_jrange_cache *jrc, bool *launch_jgc,
			unsigned long *binje_chng_zero_ents)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrange->serjio_pd;
	unsigned entry;
	unsigned long flags;
	int rv;
	bool binje_chng = binje_req_shift != jrange->binje_shift;
	DECLARE_BITMAP(zero_ent_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) = {};
	DECLARE_COMPLETION_ONSTACK(comp);
	atomic_t comp_ctr = ATOMIC_INIT(1);

	spin_lock_irqsave(&jrange->lock, flags);
	BUG_ON(jrange->status != JRANGE_RESERVED);

	if (strncmp(jrc->serjio_boot_id, jrange->serjio_pd->boot_id, NVMEIB_GID_STR_MAX) != 0) {
		_NTs(trace_serjio_upd_alloc_rng_old_boot_id, serjio_pd, "Range @JRNL_RNG_IDX - ignoring JAM Cache from older Boot ID @SERJIO_BOOT_ID (currently @BOOT_ID)",
			jrange->range_idx, jrc->serjio_boot_id, jrange->serjio_pd->boot_id);
		goto unlock_and_read;
	}
	if (jrc->rng_binje != (1U << jrange->binje_shift)) {
		_NEs(trace_serjio_upd_alloc_rng_inv_binje, serjio_pd,
				"Range @JRNL_RNG_IDX - JAM Cache has invalid N @BINJE (should be @BINJE)",
			 jrange->range_idx, jrc->rng_binje, 1 << jrange->binje_shift);
		/* This should not happen unless something has gone horribly wrong */
		BUG_ON(1);
		rv = -EINVAL;
		goto unlock_and_out;
	}
	_NTs(trace_1_serjio_upd_alloc_rng, serjio_pd, "Range @JRNL_RNG_IDX N @BINJE Entries @JENTRY_STATE_CNT - merging JAM Cache "
	NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
	" with Entries (@COUNT,@COUNT,@COUNT,@COUNT) State: " NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE "",
		 jrange->range_idx, 1 << jrange->binje_shift, jrange->n_ents, jrc->free_bmp, jrange->jentry_state_cnt[JENTRY_UNKNOWN],
	jrange->jentry_state_cnt[JENTRY_FREE], jrange->jentry_state_cnt[JENTRY_TAKEN],
	jrange->jentry_state_cnt[JENTRY_ABND], jrange->jentry_state_bmp);

	for (entry = 0; entry < jrange->n_ents; entry++) {
		enum nvmeibs_serjio_jentry_state cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(
			jrange->jentry_state_bmp, entry);
		switch (cur_jentry_state) {
		case JENTRY_FREE:
			if (!binje_chng) {
				/* Entry is FREE, move to TAKEN state */
				BUG_ON(JENTRY_STATE_CHNG(jrange, entry, JENTRY_TAKEN, true,
							 JENTRY_STATE_CHNG_REASON_ALLOC_SERJIO_FREE) != JENTRY_FREE);
			}
			break;
		case JENTRY_UNKNOWN:
			if (test_bit(entry, jrc->free_bmp) &&
				jrc->ent_md[entry].ent_gen_id == jrange->jentry_md[entry].ent_gen_id) {
				/* JAM says it was free and the GenID matches => move to TAKEN or Erase (binje_chng) */
				if (!binje_chng) {
					BUG_ON(JENTRY_STATE_CHNG(jrange, entry, JENTRY_TAKEN, true,
								 JENTRY_STATE_CHNG_REASON_ALLOC_JAM_FREE) != JENTRY_UNKNOWN);
				} else {
					set_bit(entry, zero_ent_bmp);
				}
			}
			break;
		case JENTRY_SYNCED:
			if (test_bit(entry, jrc->free_bmp) &&
				jrc->ent_md[entry].ent_gen_id == jrange->jentry_md[entry].ent_gen_id) {
				/* JAM says it was free and the GenID matches => move to TAKEN or Erase (binje_chng) */
				if (!binje_chng) {
					BUG_ON(JENTRY_STATE_CHNG(jrange, entry, JENTRY_TAKEN, true,
								 JENTRY_STATE_CHNG_REASON_ALLOC_JAM_FREE) != JENTRY_SYNCED);
				} else {
					set_bit(entry, zero_ent_bmp);
				}
			}
			break;
		case JENTRY_WAIT_RET:
		case JENTRY_TAKEN:
			/* No entries should be taken as the range was already returned */
			_NEs(error_serjio_upd_alloc_rng, serjio_pd, "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX in Invalid State @JENTRY_STATE",
				jrange->range_idx, entry, cur_jentry_state);
			BUG_ON(1);
			break;
		case JENTRY_ABND:
			if (test_bit(entry, jrc->free_bmp) &&
				jrc->ent_md[entry].ent_gen_id == jrange->jentry_md[entry].ent_gen_id) {
				/* JAM says it was FREE, but previously had reported it ABANDONED => BUG! */
				_NEs(error_1_serjio_upd_alloc_rng, serjio_pd, "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX in Invalid State @JENTRY_STATE",
					jrange->range_idx, entry, cur_jentry_state);
				BUG_ON(1);
			}
			break;
		case JENTRY_IO_ERR:
			/* Entry has IO error, nothing to do */
			break;
		default:
			BUG_ON(1);
		}
	}

unlock_and_read:
	spin_unlock_irqrestore(&jrange->lock, flags);

	for_each_set_bit(entry, zero_ent_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) {
		if ((rv = zero_journal_entry(serjio_pd, jrange, entry, &comp_ctr, &comp, zero_journal_entry_cb, (void *)JENTRY_FREE,
							"jri: %u binje: %d (gen_id: %llu) entry: %u (gen_id: %u)",
							jrange->range_idx, 1 << jrange->binje_shift,
							jrange->gen_id, entry, jrange->jentry_md[entry].ent_gen_id)) < 0) {
			_NEs(err_serjio_upd_alloc_rng_ent_zero, serjio_pd,
					"Failed (@RV) with zero_journal_entry for Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX",
					rv, jrange->range_idx, 1 << jrange->binje_shift, entry);
			JENTRY_STATE_CHNG(jrange, entry, JENTRY_IO_ERR, true, JENTRY_STATE_CHNG_REASON_ZERO_FAIL);
		}
	}
	if (atomic_dec_return(&comp_ctr) > 0)
		wait_for_completion(&comp);

	/* Sync all remaining UNKNOWN entries */
	rd_jrange(serjio_pd, sync_ent_cb, NULL, jrange, JENTRY_UNKNOWN_MASK,
			  NULL, NULL, NULL);

	//and now we will find that some unknown entries are free and need to move them to taken once again
	//but we don't held the lock, is it still valid?

	spin_lock_irqsave(&jrange->lock, flags);
	if (binje_chng) {
		if (jrange->jentry_state_cnt[JENTRY_FREE] == jrange->n_ents) {
			set_rng_binje(jrange, binje_req_shift, JENTRY_FREE, JENTRY_FREE, binje_chng_zero_ents);
		} else {
			_NTs(trace_binje_chng_rng_dirty, serjio_pd, "Range @JRNL_RNG_IDX still has @NUM_ENTS dirty entries. Cannot change N",
				jrange->range_idx, get_jentry_in_mask_cnt(jrange, JENTRY_DIRTY_MASK));
			*launch_jgc = true;
		}
	}

	for (entry = 0; entry < jrange->n_ents; entry++) {
		enum nvmeibs_serjio_jentry_state cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(
			jrange->jentry_state_bmp, entry);
		if (JENTRY_FREE == cur_jentry_state) {
			/* Entry is FREE, move to TAKEN state */
			BUG_ON(JENTRY_STATE_CHNG(jrange, entry, JENTRY_TAKEN, true,
						 JENTRY_STATE_CHNG_REASON_ALLOC_SYNC_FREE) != JENTRY_FREE);
		}
	}
	if (launch_jgc && get_jentry_in_mask_cnt(jrange, JENTRY_DIRTY_MASK) > 0)
		*launch_jgc = true;
	jrange->status = JRANGE_ALLOCATED;
	jrange->client_id = client_id;
	jrange->gen_id++;
	jrange->ent_gen_id_wrapped = false;
	getnstimeofday(&jrange->last_allocated);
	rv = 0;

unlock_and_out:
	spin_unlock_irqrestore(&jrange->lock, flags);
	return rv;
}

static void fill_rng_rsp(struct jrange_entry *rng, struct nvmeib_jrange_rsp *rsp)
{
	unsigned i;
	unsigned long flags;
	enum nvmeibs_serjio_jentry_state jentry_state;

	/* Fill in response to GET_JRANGE */
	rsp->rng_idx = rng->range_idx;
	rsp->rng_nlba = DISK_LBAS_TO_NVMEIBC_SECTS(rng->serjio_pd->di, rng->rng_nlba);
	rsp->rng_slba = DISK_LBAS_TO_NVMEIBC_SECTS(rng->serjio_pd->di, rng->serjio_pd->disk_ranges.journal.lba + rng->rng_rlba);
	spin_lock_irqsave(&rng->lock, flags);
	rsp->gen_id = rng->gen_id;
	bitmap_zero(rsp->free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	for (i = 0; i < rng->n_ents; i++) {
		union jblock_md *jmdc_entry = get_jmdc_entry(rng, i);
		jentry_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, i);
		if (GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, i) == JENTRY_TAKEN) {
			set_bit(i, rsp->free_ents_bmp);
			rsp->ent_md[i] = rng->jentry_md[i];
			BUG_ON(rng->jentry_md[i].ent_gen_id < nvmeib_jrnl_ent_gen_id_min ||
				rng->jentry_md[i].ent_gen_id > nvmeib_jrnl_ent_gen_id_max);
			/* Taken entries can either have the unused value or an io value (b/c they were free'd by JAM cache) */
			BUG_ON(!nvmeib_is_jmd_unused_entry(jmdc_entry) && !nvmeib_is_jmd_io_entry(*jmdc_entry));
		} else {
			rsp->ent_md[i].ent_gen_id = nvmeib_jrnl_ent_gen_id_invalid;
			if (JENTRY_DIRTY(jentry_state)) {
				BUG_ON(jentry_state == JENTRY_UNKNOWN); /* We need to sync every entry for JAM */
				BUG_ON(!nvmeib_is_jmd_io_entry(*jmdc_entry));
			} else {
				BUG_ON(jentry_state != JENTRY_IO_ERR);
				BUG_ON(!nvmeib_is_jmd_invalid_special(jmdc_entry));
			}
		}
		memcpy(&rsp->jmdc[i << rng->binje_shift], jmdc_entry, sizeof(*jmdc_entry) << rng->binje_shift);
	}
	for (i = (rng->n_ents << rng->binje_shift); i < NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE; i++)
		nvmeib_shared_set_jentry_md_invalid_special(&rsp->jmdc[i], 1);
	rsp->rng_binje = (1 << rng->binje_shift);
	memcpy(rsp->serjio_boot_id, rng->serjio_pd->boot_id, NVMEIB_GID_STR_MAX);
	rsp->n_ents = rng->n_ents;
	rsp->rng_nblk = rng->rng_nblk;
	rsp->max_rng_blk = rng->serjio_pd->rng_nblk;
	rsp->tot_n_rng = rng->serjio_pd->jranges_alloc_tbl.num_ranges;
	spin_unlock_irqrestore(&rng->lock, flags);
	rsp->valid  = true;
}

static DECLARE_IO_WQ_FN(io_alloc_rng_fn)
{
	int rv = -ENOENT;
	unsigned long flags;
	struct jrange_entry *ret_entry = NULL, *jrange_iter = NULL;
	struct jranges_allocation_table *jranges_alloc_tbl =
		serjio_pd ? &serjio_pd->jranges_alloc_tbl : NULL;
	enum nvmeibs_serjio_state srj_state;
	struct io_alloc_rng_param *alloc_param = param;
	u32 client_id = alloc_param->client_id;
	uuid_be client_uuid = alloc_param->client_uuid;
	const char *client_host = alloc_param->client_host;
	int binje_req_shift = alloc_param->binje_req_shift;
	const struct nvmeib_jrange_cache *jrc = alloc_param->jrc;
	struct nvmeib_jrange_rsp *rsp = alloc_param->rsp;
	u32 prev_jrnl_rng = jrc->rng_id;
	bool launch_jgc = false;
	char uuid_str[NVMEIB_GID_STR_MAX];
	DECLARE_COMPLETION_ONSTACK(comp);
	atomic_t comp_ctr = ATOMIC_INIT(1);
	bool first_alloc = true;
	/* Entries that need zeroing due to binje change */
	DECLARE_BITMAP(binje_chng_zero_ents, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) = {};
	int ctr_val;

	NFIN;
	if ((srj_state = serjio_state_get(serjio_pd)) != SERJIO_READY) {
		if (SERJIO_BOOTING(srj_state) || SERJIO_RUNNING(srj_state)) {
			_NTs(trace_serjio_5_io_alloc_rng_fn, serjio_pd, "SERJIO in busy state @SERJIO_STATE", srj_state);
			rv = -EBUSY;
		} else if (SERJIO_HALTING(srj_state)) {
			_NTs(trace_serjio_6_io_alloc_rng_fn, serjio_pd, "SERJIO in halting state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
		} else {
			_NTs(trace_serjio_7_io_alloc_rng_fn, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
			BUG_ON(1);
		}
		goto out;
	}

	if (prev_jrnl_rng != NVMEIB_EC_INVALID_JOURNAL_RANGE &&
		prev_jrnl_rng >= jranges_alloc_tbl->num_ranges) {
		_NWs(warn_serjio_io_alloc_rng_fn, serjio_pd, "Client @CLIENT_UUID (@CLIENT_HOST) had invalid previous journal range @PREV_JRNL_RNG",
			&client_uuid, client_host, prev_jrnl_rng);
		prev_jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	}

	if (prev_jrnl_rng != NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		/* Check the status of the client's previous range */
		jrange_iter = &jranges_alloc_tbl->ranges[prev_jrnl_rng];
		if (jrange_iter->status == JRANGE_DB_ERR ||	jrange_iter->status == JRANGE_DB_ZERO) {
			/* Client's previous range has a corrupt DB entry, we can rebuild it from the clients UUID */
			_NWs(warn_1_serjio_io_alloc_rng_fn, serjio_pd, "Client's @CLIENT_UUID (@CLIENT_HOST) previous range @PREV_JRNL_RNG is in error state",
				&client_uuid, client_host, prev_jrnl_rng);
			rv = -EIO;
			goto out;
		} else if ((jrange_iter->status == JRANGE_RESERVED ||
					jrange_iter->status == JRANGE_ALLOCATED) &&
					nvmeib_uuid_cmp(client_uuid, jrange_iter->client_uuid) == 0) {
			_NTs(trace_1_serjio_io_alloc_rng_fn, serjio_pd, "Found range @JRNL_RNG_IDX reserved for client @CLIENT_UUID (@CLIENT_HOST) from previous range index",
				jrange_iter->range_idx, &client_uuid, client_host);
			if (jrange_iter->status == JRANGE_ALLOCATED) {
				_NTs(trace_2_serjio_io_alloc_rng_fn, serjio_pd, "Journal range @PREV_JRNL_RNG reserved for Client @CLIENT_UUID (@CLIENT_HOST) is still in allocated state. Probably previous release hasn't finished",
				prev_jrnl_rng, &client_uuid, client_host);
				rv = -EBUSY;
				goto out;
			}
			/* Update entry */
			first_alloc = false;
			ret_entry = jrange_iter;
			upd_alloc_rng(ret_entry, client_id, binje_req_shift, jrc, &launch_jgc, binje_chng_zero_ents);
			goto write_db;
		} else {
			/* Client's previous range has either been freed or allocated to another client */
			_NTs(trace_3_serjio_io_alloc_rng_fn, serjio_pd, "Previous client range @JRNL_RNG_IDX in state @JRANGE_STATUS with client @CLIENT_UUID (@CLIENT_HOST)",
				jrange_iter->range_idx, jrange_iter->status,
				nvmeibs_serjio_uuid_to_str(uuid_str, client_uuid.b), jrange_iter->client_host);
			prev_jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
		}
	}

	spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
	if (jranges_alloc_tbl->num_reserved_ranges) {
		/* Check for the client's previous range from its UUID */
		hash_for_each_possible(jranges_alloc_tbl->reserved_ranges,
							   jrange_iter, link, hash_uuid(client_uuid)) {
			if (nvmeib_uuid_cmp(client_uuid, jrange_iter->client_uuid) == 0) {
				_NTs(trace_4_serjio_io_alloc_rng_fn, serjio_pd, "Found range @JRNL_RNG_IDX reserved for client @CLIENT_UUID (@CLIENT_HOST)",
				    jrange_iter->range_idx, &client_uuid, client_host);
				ret_entry = jrange_iter;
				break;
			}
		}
	}
	/* Client hasn't been assigned a range yet */
	if (!ret_entry) {
		if (next_free_alloc_quarantined) {
			/* [NVMESH-5297]: Testing - Allocate an invalid range to client */
			if (next_free_alloc_quarantined_idx != -1) {
				ret_entry = &jranges_alloc_tbl->ranges[next_free_alloc_quarantined_idx];
				if (ret_entry->status == JRANGE_QUARANTINED) {
					_NTs(trace_8_serjio_io_alloc_rng_fn, serjio_pd,
					     "Allocating quarantined range @JRNL_RNG_IDX to client", 
						next_free_alloc_quarantined_idx);
					hlist_del_init(&ret_entry->link);
					jranges_alloc_tbl->num_quarantined_rngs--;
					spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
					goto have_ret_entry;
				} else {
					_NTs(trace_9_serjio_io_alloc_rng_fn, serjio_pd,
						"Cannot allocate quarantined range to client - @JRNL_RNG_IDX is not quarantined",
						next_free_alloc_quarantined_idx);
				}
			} else if (jranges_alloc_tbl->quarantined_ranges.first != NULL) {
				int i;
				for (i = 0; i <= 2; i++) {
					ret_entry = hlist_entry(jranges_alloc_tbl->quarantined_ranges.first,
							struct jrange_entry, link);
					_NTs(trace_10_serjio_io_alloc_rng_fn, serjio_pd,
					     "Allocating quarantined range @JRNL_RNG_IDX to client", 
					next_free_alloc_quarantined_idx);
					hlist_del_init(&ret_entry->link);
					jranges_alloc_tbl->num_quarantined_rngs--;
					spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
					goto have_ret_entry;
				}
			}
			if (!ret_entry) {
				_NTs(trace_11_serjio_io_alloc_rng_fn, serjio_pd,
					"Cannot allocate quarantined range to client - No quarantined ranges");
			}
		}
		
		if (!jranges_alloc_tbl->num_free_rngs) {
			_NTs(trace_5_serjio_io_alloc_rng_fn, serjio_pd, "No free journal ranges to allocate to client @CLIENT_UUID (@CLIENT_HOST)",
				&client_uuid, client_host);
			/* No free ranges */
			rv = -ENOMEM;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
			goto out;
		}

		/* Get a free range from the free range list */
		BUG_ON(!jranges_alloc_tbl->free_ranges.first);
		ret_entry = hlist_entry(jranges_alloc_tbl->free_ranges.first,
								struct jrange_entry, link);

		BUG_ON(ret_entry->binje_shift != ilog2(NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY));
		BUG_ON(ret_entry->jentry_state_cnt[JENTRY_FREE] != ret_entry->n_ents);

		if (ret_entry->range_idx < NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES) {
			_NEs(err_serjio_io_alloc_rng_fn_inv_jri_free, serjio_pd,
				"Invalid range @JRNL_RNG_IDX in free-list", ret_entry->range_idx);
			BUG_NON_PRODUCTION(5279);
			/* Something has gone horribly wrong, return error */
			rv = -EFAULT;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
			goto out;
		}

		hlist_del_init(&ret_entry->link);
		jranges_alloc_tbl->num_free_rngs--;

		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
	} else {
		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
	}

have_ret_entry:
	if (ret_entry) {
		if (ret_entry->status == JRANGE_ALLOCATED) {
			_NTs(trace_6_serjio_io_alloc_rng_fn, serjio_pd, "Journal range @JRNL_RNG_IDX reserved for Client @CLIENT_UUID (@CLIENT_HOST) is still in allocated state. Probably previous release hasn't finished",
				ret_entry->range_idx, &ret_entry->client_uuid, client_host);
			first_alloc = false;
			rv = -EBUSY;
			goto out;
		} else if (ret_entry->status == JRANGE_RESERVED) {
			/* Update entry */
			first_alloc = false;
			upd_alloc_rng(ret_entry, client_id, binje_req_shift, jrc, &launch_jgc, binje_chng_zero_ents);
			goto write_db;
		} else if (ret_entry->status == JRANGE_FREE || ret_entry->status == JRANGE_QUARANTINED) {
			if (ret_entry->status == JRANGE_QUARANTINED) {
				_NWs(warn_2_serjio_io_alloc_rng_fn, serjio_pd,
				     "Allocating QUARANTINED range @JRNL_RNG_IDX to client @CLIENT_UUID (@CLIENT_HOST)",
				     ret_entry->range_idx, &ret_entry->client_uuid, ret_entry->client_host);
				BUG_ON(!next_free_alloc_quarantined);
				next_free_alloc_quarantined = false;
			}
			/* Set free range to allocated to the client */
			alloc_rng_to_client(serjio_pd, ret_entry, client_id, client_uuid, client_host, binje_req_shift, binje_chng_zero_ents);
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			hash_add(jranges_alloc_tbl->reserved_ranges,
						&ret_entry->link, hash_uuid(client_uuid));
			jranges_alloc_tbl->num_reserved_ranges++;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		} else if (ret_entry->status == JRANGE_RSVD_DB_ERR) {
			_NEs(error_4_serjio_io_alloc_rng_fn, serjio_pd, "Journal range @JRNL_RNG_IDX reserved for Client @CLIENT_UUID (@CLIENT_HOST) in error state @JRANGE_STATUS",
				ret_entry->range_idx, &ret_entry->client_uuid, ret_entry->client_host, ret_entry->status);
			rv = -EIO;
			goto out;
		} else {
			_NEs(error_serjio_io_alloc_rng_fn, serjio_pd, "Journal range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
				ret_entry->range_idx, ret_entry->status);
			BUG_ON(1);
		}
	}

	_NTs(trace_7_serjio_io_alloc_rng_fn, serjio_pd, "Allocated free journal range @JRNL_RNG_IDX to client @CLIENT_UUID (@CLIENT_ID_INT)",
		ret_entry->range_idx, &client_uuid, client_id);

write_db:
	/* Write the allocation to the SERJIO DB (and wait for completion) */
	if ((rv = write_jrange_to_serjio_db(ret_entry, &comp, &comp_ctr))) {
		_NEs(error_1_serjio_io_alloc_rng_fn, serjio_pd,
				"Failed (@RV) writing range @JRNL_RNG_IDX to SERJIO DB for client @CLIENT_UUID (@CLIENT_ID_INT)",
				rv, ret_entry->range_idx, &client_uuid, client_id);
		/* Failed to write to disk - move to db error state if first allocation or reserved db error state if non-first */
		spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
		if (first_alloc) {
			hlist_del(&ret_entry->link);
			jranges_alloc_tbl->num_reserved_ranges--;
			ret_entry->status = JRANGE_DB_ERR;
			hlist_add_head(&ret_entry->link, &jranges_alloc_tbl->db_err_ranges);
			jranges_alloc_tbl->num_db_err_rngs++;
		} else {
			_NEs(error_2_serjio_io_alloc_rng_fn, serjio_pd,
					"Failed to write to DB for non-first allocation of range @JRNL_RNG_IDX for client @CLIENT_UUID (@CLIENT_ID_INT)."
					" Client will be stuck without a range!", ret_entry->range_idx, &client_uuid, client_id);
			ret_entry->status = JRANGE_RSVD_DB_ERR;
		}
		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		rv = -EIO;
		goto out;
	}

	if (stamp_jrnl_entries) {
		unsigned i;
		/* Mark all taken entries for debug purposes */
		for (i = 0; i < ret_entry->n_ents; i++) {
			if (GET_JENTRY_STATE_FROM_BMP(ret_entry->jentry_state_bmp, i) == JENTRY_TAKEN) {
				if ((rv = zero_journal_entry(serjio_pd, ret_entry, i, &comp_ctr, &comp, zero_journal_entry_cb, (void *)JENTRY_TAKEN,
								"client: %s uuid: %pUB cid: %u jri: %u binje: %d (gen_id: %llu) entry: %u/%u (gen_id: %u)",
								client_host, &client_uuid, client_id, ret_entry->range_idx, 1 << ret_entry->binje_shift,
								ret_entry->gen_id, i, ret_entry->n_ents, ret_entry->jentry_md[i].ent_gen_id)) < 0)
				{
					union jblock_md *jmdc_entry = get_jmdc_entry(ret_entry, i);

					_NEs(error_3_serjio_io_alloc_rng_fn, serjio_pd,
					     "Failed (@RV) to zero entry @JRNL_RNG_ENT_IDX of range @JRNL_RNG_IDX. Aborting allocate range",
					     rv, i, ret_entry->range_idx);

					BUG_ON(JENTRY_STATE_CHNG(ret_entry, i, JENTRY_IO_ERR, false,
								 JENTRY_STATE_CHNG_REASON_ZERO_FAIL) != JENTRY_TAKEN);
					nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << ret_entry->binje_shift);

					/* Allow the allocate to succeed even if some of the entries are unusable */
					rv = 0;
				}
			}
		}
	} else {
		unsigned i;
		/* We might still need to zero entries due to binje change */
		for_each_set_bit(i, binje_chng_zero_ents, ret_entry->n_ents) {
			enum nvmeibs_serjio_jentry_state ent_state = GET_JENTRY_STATE_FROM_BMP(ret_entry->jentry_state_bmp, i);
			SERJIO_BUG_ON(ent_state != JENTRY_TAKEN,
				      bug_io_alloc_rng_fn_binje_chng_not_taken, serjio_pd,
					"Range @JRNL_RNG_IDX Entry @JENT_IDX has invalid state @JENTRY_STATE",
					ret_entry->range_idx, i, ent_state);
			if ((rv = zero_journal_entry(serjio_pd, ret_entry, i, &comp_ctr, &comp, zero_journal_entry_cb, (void *)JENTRY_TAKEN,
					"client: %s uuid: %pUB cid: %u jri: %u binje: %d (gen_id: %llu) entry: %u/%u (gen_id: %u)",
					client_host, &client_uuid, client_id, ret_entry->range_idx, 1 << ret_entry->binje_shift,
					ret_entry->gen_id, i, ret_entry->n_ents, ret_entry->jentry_md[i].ent_gen_id)) < 0)
			{
				union jblock_md *jmdc_entry = get_jmdc_entry(ret_entry, i);

				_NEs(error_5_serjio_io_alloc_rng_fn, serjio_pd,
				     "Failed (@RV) to zero entry @JRNL_RNG_ENT_IDX of range @JRNL_RNG_IDX. Aborting allocate range",
				     rv, i, ret_entry->range_idx);

				BUG_ON(JENTRY_STATE_CHNG(ret_entry, i, JENTRY_IO_ERR, false,
							 JENTRY_STATE_CHNG_REASON_ZERO_FAIL) != JENTRY_TAKEN);
				nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << ret_entry->binje_shift);

				/* Allow the allocate to succeed even if some of the entries are unusable */
				rv = 0;
			}
		}
	}

	if ((ctr_val = atomic_dec_return(&comp_ctr))) {
		_NDs(trace_io_alloc_rng_fn_trace_comp_ctr, serjio_pd, "comp_ctr: @COUNT", ctr_val);
		wait_for_completion(&comp);
	} else {
		_NDs(trace_io_alloc_rng_fn_trace_comp_ctr_2, serjio_pd, "comp_ctr: @COUNT", ctr_val);
	}

	if (ret_entry->status == JRANGE_RSVD_DB_ERR) {
		if (first_alloc) {
			/* For first alloc, we can isolate this range and when the client rediscovers it will get a new range */
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			hlist_del(&ret_entry->link);
			jranges_alloc_tbl->num_reserved_ranges--;
			ret_entry->status = JRANGE_DB_ERR;
			hlist_add_head(&ret_entry->link, &jranges_alloc_tbl->db_err_ranges);
			jranges_alloc_tbl->num_db_err_rngs++;
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		} else {
			_NEs(err_io_alloc_rng_fn_write_db_fail, serjio_pd,
					"Failed to write to DB for non-first allocation of range @JRNL_RNG_IDX to DB for client @CLIENT_UUID (@CLIENT_ID_INT)."
						" Client will be stuck without a range!", ret_entry->range_idx, &client_uuid, client_id);
		}
		rv = -EIO;
	}

	sync_jmdc_range_to_all_nics(serjio_pd, ret_entry->range_idx);

	if (launch_jgc) {
		/* Request TOMA launch a new JGC.
		 *	This will include a tag for each range that needs cleaning */
		chk_launch_new_jgc(serjio_pd, false);
	}
	nvmeib_ref_init(&ret_entry->alloc_ref);
	rv = ret_entry->range_idx;

out:
	if (rv < 0)
		rsp->valid = false;
	else
		fill_rng_rsp(ret_entry, rsp);

	NFOUT;
	return rv;
}

int nvmeibs_serjio_alloc_journal_range(struct nvmeibs_disk_info* di, u32 client_id,
				       uuid_be client_uuid, const char *client_host,
					   const binje_t binje_req, const struct nvmeib_jrange_cache *jrc,
					   struct nvmeib_jrange_rsp *rsp)
{
	int rv = 0;
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	int binje_req_shift = ilog2(binje_req);

	NFIN;

	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_alloc_journal_range, "SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}
	if ((1U << binje_req_shift) != binje_req || binje_req < NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY || binje_req > NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY) {
		_NEs(error_serjio_alloc_journal_range_invalid_binje, serjio_pd, "N @BINJE requested is not valid",
			 binje_req);
		rv = -EINVAL;
		goto out;
	}
	rv = alloc_journal_range(serjio_pd, client_id, client_uuid,
							 client_host, binje_req_shift, jrc, rsp);

out:
	NFOUT;
	return rv;
}

static void scan_ret_rng(struct jrange_entry *jrng)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrng->serjio_pd;
	unsigned i;
	enum nvmeibs_serjio_jentry_state cur_jentry_state,  next_jentry_state;
	unsigned long flags;
	struct seg_tree_entry *seg_tree_entry;
	enum nvmeibs_serjio_jentry_state_chng_reason chng_reason = JENTRY_STATE_CHNG_REASON_UNKNOWN;

	NFIN;
	spin_lock_irqsave(&jrng->lock, flags);
	for (i = 0; i < jrng->n_ents; i++) {
		union jblock_md *jmdc_entry = get_jmdc_entry(jrng, i);
		cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, i);
		if (cur_jentry_state == JENTRY_TAKEN) {
			if (nvmeib_is_jmd_io_entry(*jmdc_entry)) {
				u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
				u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
				int chain_err = NVMEIB_JENTRY_CHAIN_OK;
				next_jentry_state = JENTRY_UNKNOWN;
				chng_reason = JENTRY_STATE_CHNG_REASON_RET_DIRTY;
				if ((chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << jrng->binje_shift, &j2d_start, &j2d_end)) != NVMEIB_JENTRY_CHAIN_OK) {
					_NWs(serjio_scan_ret_rng_jmdc_inv_chain, serjio_pd,
						 "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX has invalid chain at @PTR. Chain Error @ERR_STR Chain Error Block @IDX",
						jrng->range_idx, i, jmdc_entry,
						nvmeib_shared_jentry_md_chain_err_str(chain_err),
						nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
					goto state_change;
				}
				seg_tree_entry = seg_tree_iter_first(&serjio_pd->jrnl_seg_rb_root, j2d_start, j2d_start);
				if (!seg_tree_entry) {
					_NWs(serjio_scan_ret_rng_j2d_invalid, serjio_pd,
						 "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX has invalid J2D [@J2D_START,@J2D_END]",
						jrng->range_idx, i, j2d_start, j2d_end);
					goto state_change;
				}
				SERJIO_BUG_ON(j2d_end > seg_tree_entry->elba,
							err_serjio_scan_ret_rng_j2d_overflow, serjio_pd,
							"Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX J2D [@J2D_START,@J2D_END] "
							" overflows segment @SEG_UUID_STR [@SLBA_LLONG, @ELBA]",
							jrng->range_idx, 1 << jrng->binje_shift, i, j2d_start, j2d_end,
							seg_tree_entry->seg_uuid_str, seg_tree_entry->slba, seg_tree_entry->elba);
				if (seg_tree_entry->deprecated &&
					seg_tree_entry->cln_state != CLN_SEG_WAIT_RNG_RETURN) {
					_NWs(serjio_scan_ret_rng_deprecated_seg, serjio_pd,
						 "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX J2D [@J2D_START,@J2D_END] points to"
						" @JRNL_SEG_STATUS Segment @SEG_UUID_STR",
						jrng->range_idx, i, j2d_start, j2d_end, "DEPRECATED", seg_tree_entry->seg_uuid_str);
					if (seg_tree_entry->cln_state == CLN_SEG_DONE)
						seg_tree_entry->cln_state = CLN_SEG_IDLE;
					} else if (seg_tree_entry->seg_delete &&
						seg_tree_entry->cln_state != CLN_SEG_WAIT_RNG_RETURN) {
					_NWs(serjio_scan_ret_rng_seg_delete, serjio_pd,
						 "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX J2D [@J2D_START,@J2D_END] points to"
						" Segment @SEG_UUID_STR after TOMA requested clean for delete",
						jrng->range_idx, i, j2d_start, j2d_end, seg_tree_entry->seg_uuid_str);
					if (seg_tree_entry->cln_state == CLN_SEG_DONE)
						seg_tree_entry->cln_state = CLN_SEG_IDLE;
				}
			} else if (nvmeib_is_jmd_unused_entry(jmdc_entry)) {
				next_jentry_state = JENTRY_FREE;
				chng_reason = JENTRY_STATE_CHNG_REASON_RET_FREE;
			} else if (nvmeib_is_jmd_invalid_special(jmdc_entry)) {
				next_jentry_state = JENTRY_IO_ERR;
				chng_reason = JENTRY_STATE_CHNG_REASON_RET_INVALID;
				_NTs(trace_serjio_scan_ret_rng_ent_inv_special, jrng->serjio_pd,
					"Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX has Invalid Special Value",
					jrng->range_idx, i);
			} else {
				next_jentry_state = JENTRY_IO_ERR;
				chng_reason = JENTRY_STATE_CHNG_REASON_RET_INVALID;
				SERJIO_BUG_ON(1, error_serjio_scan_ret_rng_ret_ent_inv_jmdc, jrng->serjio_pd,
							"Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX "
							"- JMDC has invalid value @JMDC_ENT\n",
							jrng->range_idx, i, jmdc_entry->raw);
			}
state_change:
			JENTRY_STATE_CHNG(jrng, i, next_jentry_state, true, chng_reason);
		}
	}
	spin_unlock_irqrestore(&jrng->lock, flags);

	NFOUT;
}

static DECLARE_IO_WQ_FN(io_ret_rng_fn)
{
	struct jrange_entry* jrange;
	int rv = 0;
	struct jranges_allocation_table *jranges_alloc_tbl =
		serjio_pd ? &serjio_pd->jranges_alloc_tbl : NULL;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	unsigned long flags;
	long rng_param = (long)param;
	u32 range_idx;

	NFIN;
	if (srj_state != SERJIO_READY && srj_state != SERJIO_GPT_UPDATE && srj_state != SERJIO_CLN_JRNL) {
		_NTs(trace_serjio_io_ret_rng_fn, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
		rv = -ECANCELED;
		goto out;
	}
	if (rng_param > jranges_alloc_tbl->num_ranges) {
		_NEs(error_serjio_io_ret_rng_fn, serjio_pd, "Invalid Journal Range @RNG_PARAM", rng_param);
		rv = -EINVAL;
		goto out;
	}
	range_idx = rng_param;
	_NT(trace_4_serjio_io_ret_rng_fn, "range_idx @JRNL_RNG_IDX", range_idx);
	jrange = &jranges_alloc_tbl->ranges[range_idx];

	if (jrange->status != JRANGE_ALLOCATED) {
		_NEs(error_1_serjio_io_ret_rng_fn, serjio_pd, "Journal Range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
		   range_idx, jrange->status);
		rv = -EINVAL;
		goto out;
	}

	_NTs(trace_3_serjio_io_ret_rng_fn, serjio_pd,
		 "Waiting for all handles @JRANGE_HANDLE for range @JRNL_RNG_IDX to release (ref_cnt @REF_CNT)",
		 (void *)jrange, jrange->range_idx, nvmeib_ref_read(&jrange->alloc_ref));
	nvmeib_ref_release_start(&jrange->alloc_ref);
	nvmeib_ref_release_wait(&jrange->alloc_ref);

	/* Scan JMDC to see which entries are abandoned */
	getnstimeofday(&jrange->last_returned);
	scan_ret_rng(jrange);

	if (srj_state == SERJIO_CLN_JRNL) {
		/* The scan may have freed some entires -
		*	check to see if they are relevant for a
		* pending clean disk range operation */
		chk_wait_ret_cln_disk_rng(jrange);
	}

	_NTs(trace_2_serjio_io_ret_rng_fn, serjio_pd, "Range @JRNL_RNG_IDX (Client @CLIENT_UUID) return. N: @BINJE Entries: @ENTRIES - (Free: @ENTRIES, Abnd: @ENTRIES, Unknown: @ENTRIES)",
		jrange->range_idx, &jrange->client_uuid, 1 << jrange->binje_shift, jrange->n_ents,
		jrange->jentry_state_cnt[JENTRY_FREE],
		jrange->jentry_state_cnt[JENTRY_ABND],
		jrange->jentry_state_cnt[JENTRY_UNKNOWN]);

	spin_lock_irqsave(&jrange->lock, flags);
	getnstimeofday(&jrange->last_returned);
	jrange->status = JRANGE_RESERVED;
	jrange->returned_jif = jiffies;

	if (jrange->jentry_state_cnt[JENTRY_FREE] == jrange->n_ents) {
		spin_unlock_irqrestore(&jrange->lock, flags);
		free_jrnl_rng(jrange, NULL, NULL);
	} else {
		if (jrange->ent_gen_id_wrapped) {
			jrange->gen_id++;
			jrange->ent_gen_id_wrapped = false;
		}
		spin_unlock_irqrestore(&jrange->lock, flags);
		if (!IS_SERJIO_DYING(serjio_pd) && !timer_pending(&serjio_pd->jgc_timer))
			mod_timer(&serjio_pd->jgc_timer, jiffies + NVMEIB_JGC_RNG_RECONNECT_TIMEOUT);
		/* Write the state back to the Database */
		if ((rv = write_jrange_to_serjio_db(jrange, NULL, NULL))) {
			_NEs(error_2_serjio_io_ret_rng_fn, serjio_pd, "Failed (@RV) to write journal range @JRNL_RNG_IDX data to SERJIO DB",
				rv, jrange->range_idx);
			spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
			if (jrange->status == JRANGE_FREE) {
				/* Range was freed - move to DB error state */
				hlist_del(&jrange->link);
				jranges_alloc_tbl->num_reserved_ranges--;
				jrange->status = JRANGE_DB_ERR;
				hlist_add_head(&jrange->link, &jranges_alloc_tbl->db_err_ranges);
				jranges_alloc_tbl->num_db_err_rngs++;
			} else {
				/* Range is still allocated to client - move to reserved DB error state. Client will be stuck without a range */
				_NEs(error_3_serjio_io_ret_rng_fn, serjio_pd,
						"Failed to write range @JRNL_RNG_IDX to DB. "
						"@CLIENT_UUID (@CLIENT_HOST) will be stuck without a range",
						jrange->range_idx, &jrange->client_uuid, jrange->client_host);
				jrange->status = JRANGE_RSVD_DB_ERR;
			}
			spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
		}
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_return_journal_range(struct nvmeibs_disk_info *di, u32 cl_jrnl_rng)
{
	int rv = 0;
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);

	NFIN;
	if (!serjio_pd) {
		_NT(trace_serjio_nvmeibs_serjio_return_journal_range, "SERJIO: Disk has no private data");
		rv = -ENODATA;
		goto out;
	}
	rv = return_journal_range(serjio_pd, cl_jrnl_rng);
out:
	NFOUT;
	return rv;
}

/* Called by the NR Channel to set the JMDC as PB IO or for JENTRY ERASE.
 * Checks the Entry State is JENTRY_TAKEN or JENTRY_WAIT_RET */
int nvmeibs_serjio_jmdc_entry_set(void *jrange_handle, u64 rng_gen_id,
								  int n_ent, u16 *ent_idx,
								  const struct nvmeib_jrnl_ent_md *ent_md,
								  const union jblock_md *ent_data,
								  bool rdma_sync)
{
	struct jrange_entry *rng = jrange_handle;
	int rv = 0, i;
	unsigned long flags;
	union jblock_md *jmdc_entry;
	enum nvmeibs_serjio_jentry_state jentry_state;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;

	if (!rng) {
		_NE(error_serjio_jmdc_entry_set_null_handle, "NULL jrange_handle");
		rv = -EINVAL;
		goto out;
	}
	serjio_pd = rng->serjio_pd;

	spin_lock_irqsave(&rng->lock, flags);
	if (rng_gen_id != 0 && rng_gen_id != rng->gen_id) {
		_NTs(error_nvmeibs_serjio_c_5416, serjio_pd, "Range @JRNL_RNG_IDX GenID Mismatch: "
		"@JRNL_RNG_GEN != @JRNL_RNG_GEN. Probably an Entry GenID has wrapped", rng->range_idx, rng_gen_id, rng->gen_id);
		/* It can only lag due to A2F messages in the pipe */
		SERJIO_BUG_ON(rng_gen_id > rng->gen_id,
					error_nvmeibs_serjio_jmdc_entry_set_rng_id_leading, serjio_pd,
					"Range: @JRNL_RNG_IDX - JAM GenID: @JRNL_RNG_GEN > SERJIO GenID: @JRNL_RNG_GEN",
					rng->range_idx, rng_gen_id, rng->gen_id);
		rv = -EAGAIN;
		goto unlock;
	}
	for (i = 0; i < n_ent; i++) {
		if (ent_idx[i] >= rng->n_ents) {
			spin_unlock_irqrestore(&rng->lock, flags);
			_NEs(error_3_serjio_nvmeibs_serjio_jmdc_entry_set, serjio_pd, "Invalid entry @INDEX", ent_idx[i]);
			rv = -EINVAL;
			goto out;
		}
		if (ent_md && ent_md[i].ent_gen_id != rng->jentry_md[ent_idx[i]].ent_gen_id) {
			_NTs(error_nvmeibs_serjio_c_5415, serjio_pd,
				 "Entry @JRNL_RNG_ENT_IDX of Range @JRNL_RNG_IDX GenID Mismatch: @JRNL_RNG_ENT_GEN != @JRNL_RNG_ENT_GEN",
					ent_idx[i], rng->range_idx, ent_md[i].ent_gen_id, rng->jentry_md[ent_idx[i]].ent_gen_id);
			rv = -EINVAL;
			goto unlock;
		}
		jentry_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, ent_idx[i]);
		SERJIO_BUG_ON(!JENTRY_OWNED_BY_JAM(jentry_state),
					error_nvmeibs_serjio_jmdc_entry_set_inv_ent_state, serjio_pd,
					"Entry @JRNL_RNG_ENT_IDX of Range @JRNL_RNG_IDX"
					" in invalid state @JENTRY_STATE GenID @JRNL_RNG_GEN.@JRNL_RNG_ENT_GEN",
					ent_idx[i], rng->range_idx, jentry_state, rng->gen_id, rng->jentry_md[ent_idx[i]].ent_gen_id);
		jmdc_entry = get_jmdc_entry(rng, ent_idx[i]);
		memcpy(jmdc_entry, &ent_data[i << rng->binje_shift], sizeof(*jmdc_entry) << rng->binje_shift);
	}
unlock:
	spin_unlock_irqrestore(&rng->lock, flags);

	if (rdma_sync)
		sync_jmdc_range_to_all_nics(serjio_pd, rng->range_idx);

out:
	return rv;
}

static int get_jmdc_dev_map(struct nvmeibs_serjio_disk_private_data *serjio_pd,
			    struct nvmeibs_dev *dev, void **jmdc_map_handle);

void *nvmeibs_serjio_get_jrange_handle(struct nvmeibs_disk_info *di, u32 cid,
									   u32 jrnl_rng, struct nvmeibs_dev *dev,
									   void **jmdc_map_handle)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = di ?
		nvmeibs_disk_info_get_serjio_private_data(di) : NULL;

	struct jrange_entry *jrange_entry = NULL;
	int rv_int;
	void *rv = ERR_PTR(-ENOENT);
	unsigned long flags;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	unsigned long ts;
	int ref_cnt;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_get_jmdc_rai,
			"Could not get access to SERJIO private data for client @CID", cid);
		rv = ERR_PTR(-ENODATA);
		goto out;
	}
	if (!SERJIO_RUNNING(srj_state)) {
		_NEs(error_1_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd,
			 "SERJIO is not ready. State: @SERJIO_STATE", srj_state);
		rv = ERR_PTR(-EAGAIN);
		goto out;
	}
	if ((int)jrnl_rng < 0) {
		_NEs(error_2_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd, "Client @CID has not been assigned a journal range",
			cid);
		rv = ERR_PTR(-EINVAL);
		goto out;
	}
	if (jrnl_rng > serjio_pd->jranges_alloc_tbl.num_ranges) {
		_NEs(error_3_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd, "Client @CID has invalid journal range @JRNL_RNG_IDX/@NUM_RANGES",
		    cid, jrnl_rng, serjio_pd->jranges_alloc_tbl.num_ranges);
		rv = ERR_PTR(-EINVAL);
		goto out;
	}
	jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[jrnl_rng];
	ts = jiffies;
	spin_lock_irqsave(&jrange_entry->lock, flags);
	if (jrange_entry->status != JRANGE_ALLOCATED || jrange_entry->client_id != cid) {
		spin_unlock_irqrestore(&jrange_entry->lock, flags);
		_NEs(error_4_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd,
			 "Client @CID has incorrect journal range @JRNL_RNG_IDX",
			cid, jrnl_rng);
		rv = ERR_PTR(-EINVAL);
		goto out;
	}
	if (!(ref_cnt = nvmeib_ref_get(&jrange_entry->alloc_ref))) {
		spin_unlock_irqrestore(&jrange_entry->lock, flags);
		_NEs(error_5_serjio_nvmeibs_serjio_get_jrange_handle_no_get_ref, serjio_pd,
			 "Client @CID could not get handle to journal range @JRNL_RNG_IDX",
			 cid, jrnl_rng);
		rv = ERR_PTR(-EBUSY);
		goto out;
	}
	spin_unlock_irqrestore(&jrange_entry->lock, flags);
	_NTs(trace_1_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd,
		 "Got handle @JRANGE_HANDLE for range @JRNL_RNG_IDX (ref_cnt @REF_CNT,"
		 "took=@LLU)",
		 (void*)jrange_entry, jrange_entry->range_idx, ref_cnt, jiffies - ts);
	rv = jrange_entry;

	if (!jmdc_map_handle)
		goto out;

#if !JMDC_MAP_PER_CLIENT
	if ((rv_int = get_jmdc_dev_map(serjio_pd, dev, jmdc_map_handle)) < 0) {
		_NEs(error_5_serjio_nvmeibs_serjio_get_jmdc_rai, serjio_pd, "jmdc mapping not found on nic @IB_DEV_NAME", N2IB(dev)->name);
		nvmeibs_serjio_put_jrange_handle(jrange_entry, NULL);
		rv = ERR_PTR(-EFAULT);
	}
#else
	/* TBD: Need to decide whether to use a page per journal range (instead of 1/4 page),
	modify alloc_n_map to allow shorter MRs
	or to use nvmeib_map_fr (but then we need an iu and a net) */
	BUILD_BUG_ON(1);
#endif

out:
	NFOUT;
	return rv;
}

void nvmeibs_serjio_put_jrange_handle(void *rng_handle, void *jmdc_map_handle)
{
	struct jrange_entry *jrng = rng_handle;
	if (!IS_ERR_OR_NULL(jrng)) {
		int ref_cnt = nvmeib_ref_put(&jrng->alloc_ref);
		_NTs(trace_serjio_put_jrange_handle, jrng->serjio_pd,
			 "Putting handle @JRANGE_HANDLE for range @JRNL_RNG_IDX (ref_cnt @REF_CNT)",
			 rng_handle, jrng->range_idx, ref_cnt);
	}
	if (jmdc_map_handle)
		nvmeibs_serjio_put_jmdc_dev_map(jmdc_map_handle);
}

static int get_jmdc_dev_map(struct nvmeibs_serjio_disk_private_data *serjio_pd,
			    struct nvmeibs_dev *dev, void **jmdc_map_handle)
{
	struct jmdc_mapping* jmdc = NULL;
	int rv = -ENOENT;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&serjio_pd->jmdc_mem_lock, flags);
	list_for_each_entry(jmdc, &serjio_pd->jmdc_mems, link) {
		if (jmdc->nic_dev == dev) {
			_NTs(trace_serjio_nvmeibs_serjio_get_jmdc_dev_map, serjio_pd,
				"Getting jmdc mapping on nic @IB_DEV_NAME "
				"(lkey: @LKEY, rkey: @RKEY, addr: @IOADDR, pages: @N_PAGES).",
				N2IB(dev)->name, jmdc->jmdc_area_map.lkey, jmdc->jmdc_area_map.rkey,
				jmdc->jmdc_area_map.ioaddr, jmdc->jmdc_area_map.n_pages);
			if (nvmeib_ref_get(&jmdc->refcount)) {
				*jmdc_map_handle = jmdc;
				rv = 0;
			}
			break;
		}
	}
	spin_unlock_irqrestore(&serjio_pd->jmdc_mem_lock, flags);
	if (rv == -ENOENT)
		_NEs(error_2_serjio_nvmeibs_serjio_get_jmdc_dev_map, serjio_pd, "jmdc mapping not found on nic @IB_DEV_NAME", N2IB(dev)->name);

	NFOUT;
	return rv;
}

/* DEPRECATED: GET_JMDC now makes a copy of the JMDC to the BB before transmission */
int __attribute__((unused)) nvmeibs_serjio_get_jmdc_dev_map(struct nvmeibs_disk_info *di,
				    struct nvmeibs_dev *dev,
				    void **jmdc_map_handle)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
	nvmeibs_disk_info_get_serjio_private_data(di);
	int rv;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_get_jmdc_dev_map, "Could not get access to SERJIO private data");
		rv = -ENODATA;
		goto out;
	}
	if (!IS_SERJIO_READY(serjio_pd)) {
		_NEs(error_1_serjio_nvmeibs_serjio_get_jmdc_dev_map, serjio_pd, "Journal is not ready");
		rv = -EAGAIN;
		goto out;
	}
	rv = get_jmdc_dev_map(serjio_pd, dev, jmdc_map_handle);
out:
	NFOUT;
	return rv;
}

void nvmeibs_serjio_put_jmdc_dev_map(void *jmdc_map_handle)
{
	struct jmdc_mapping* jmdc_map = jmdc_map_handle;

	NFIN;
	_NTs(trace_serjio_nvmeibs_serjio_put_jmdc_dev_map, jmdc_map->serjio_pd,
	     "Putting jmdc mapping on nic @IB_DEV_NAME "
		"(lkey: @LKEY, rkey: @RKEY, addr: @IOADDR, pages: @N_PAGES).",
	     N2IB(jmdc_map->nic_dev)->name, jmdc_map->jmdc_area_map.lkey, jmdc_map->jmdc_area_map.rkey,
	     jmdc_map->jmdc_area_map.ioaddr, jmdc_map->jmdc_area_map.n_pages);

	nvmeib_ref_put(&jmdc_map->refcount);
	NFOUT;
}

int nvmeibs_serjio_get_jrange_jmdc_rai(void *rng_handle, void *jmdc_map_handle, struct nvmeib_remote_access_info *jmdc_rai)
{
	struct jrange_entry *jrng = rng_handle;
	struct jmdc_mapping *jmdc_map = jmdc_map_handle;
	int rv;

	NFIN;
	if (!rng_handle) {
		_NE(err_serjio_get_jrange_jmdc_rai_null_rng_handle, "NULL Range Handle");
		rv = -EINVAL;
		goto out;
	}
	if (!jmdc_map_handle) {
		_NE(err_serjio_get_jrange_jmdc_rai_null_map_handle, "NULL JMDC Map Handle");
		rv = -EINVAL;
		goto out;
	}

#if !JMDC_MAP_PER_CLIENT
	jmdc_rai->rkey = jmdc_map->jmdc_area_map.rkey;
	jmdc_rai->raddr = jmdc_map->jmdc_area_map.ioaddr +
		(jrng->rng_rblk * sizeof(union jblock_md));
	jmdc_rai->len = jrng->rng_nblk * sizeof(union jblock_md);
	if (jmdc_rai->raddr + jmdc_rai->len > jmdc_map->jmdc_area_map.ioaddr + (jmdc_map->jmdc_area_map.n_pages << PAGE_SHIFT)) {
		_NW(warn_serjio_nvmeibs_serjio_get_jmdc_rai,
		    "raddr @RADDR + len @LEN > ioaddr @IOADDR + n_pages @N_PAGES for journal range @JRNL_RNG_IDX",
		    jmdc_rai->raddr, jmdc_rai->len, jmdc_map->jmdc_area_map.ioaddr, jmdc_map->jmdc_area_map.n_pages, jrng->range_idx);
		rv = -EFAULT;
		goto out;
	}
	rv = 0;
#else
	BUILD_BUG_ON(1);
	rv = -ENOTSUPP;
#endif

out:
	NFOUT;
	return rv;
}

static int send_jam_abnd_free(struct nvmeibs_serjio_disk_private_data *serjio_pd,
			   struct jrange_entry *jrange_entry, unsigned long *abnd_free_bmp)
{
	int rv = 0;

	/* Send a new update to the client's JAM */
	if (!(rv = nvmeibs_serjio_send_journal_abnd_free(serjio_pd->di, jrange_entry->client_id,
		jrange_entry->range_idx, 1U << jrange_entry->binje_shift, jrange_entry->gen_id, abnd_free_bmp, jrange_entry->jentry_md))) {
		_NTs(trace_serjio_send_jam_abnd_free, serjio_pd, "Sent JAM Update with A2F bmp: "
			NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE "", abnd_free_bmp);
	} else
		_NEs(error_serjio_send_jam_abnd_free, serjio_pd, "nvmeibs_serjio_send_journal_abnd_free failed (@RV)", rv);
	return rv;
}

static int chk_rng_post_free(struct jrange_entry *jrange_entry, struct completion *comp, atomic_t *ctr,
			     enum nvmeibs_serjio_jentry_state_chng_reason chng_ent_reason)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrange_entry->serjio_pd;
	int rv = 0;
	unsigned long flags;
	unsigned entry;
	enum nvmeibs_serjio_jentry_state prev_state;
	DECLARE_BITMAP(abnd_free_ents, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) = {0};

	NFIN;
	spin_lock_irqsave(&jrange_entry->lock, flags);
	if (jrange_entry->status == JRANGE_RESERVED) {
		/* Journal range is not allocated to a client
		Check the abandoned_entries. If they are all free, we can free the range */
		_NTs(trace_serjio_chk_rng_post_free, serjio_pd, "Journal range @JRNL_RNG_IDX Client @CLIENT_UUID is not connected",
		    jrange_entry->range_idx, &jrange_entry->client_uuid);
		if (jrange_entry->jentry_state_cnt[JENTRY_FREE] == jrange_entry->n_ents) {
			/* All entries are free, range can be returned to free state */
			_NTs(trace_1_serjio_chk_rng_post_free, serjio_pd, "All Journal range @JRNL_RNG_IDX entries free. Freeing range",
				jrange_entry->range_idx);
			spin_unlock_irqrestore(&jrange_entry->lock, flags);
			free_jrnl_rng(jrange_entry, comp, ctr);
		}
		else {
			/* Not updating the DB. There are still entries to be cleaned */
			_NTs(trace_2_serjio_chk_rng_post_free, serjio_pd, "Journal Range @JRNL_RNG_IDX N @BINJE Entries @ENTRIES (Client @CLIENT_UUID) - "
			"(Remaining @JENTRY_STATE @JENTRY_STATE_CNT @JENTRY_STATE @JENTRY_STATE_CNT @JENTRY_STATE @JENTRY_STATE_CNT)",
				jrange_entry->range_idx, 1 << jrange_entry->binje_shift,
				jrange_entry->n_ents, &jrange_entry->client_uuid,
				JENTRY_ABND, jrange_entry->jentry_state_cnt[JENTRY_ABND],
				JENTRY_UNKNOWN, jrange_entry->jentry_state_cnt[JENTRY_UNKNOWN],
				JENTRY_SYNCED, jrange_entry->jentry_state_cnt[JENTRY_SYNCED]);
			/* Check to see if we need to update the generation ID */
			if (jrange_entry->ent_gen_id_wrapped) {
				jrange_entry->gen_id++;
				jrange_entry->ent_gen_id_wrapped = false;
			}
			spin_unlock_irqrestore(&jrange_entry->lock, flags);
			if ((rv = write_jrange_to_serjio_db(jrange_entry, comp, ctr)) < 0) {
				_NEs(chk_rng_post_free_e1, serjio_pd, "write_jrange_to_serjio_db failed (@INT) for range @UINT",
					rv, jrange_entry->range_idx);
			}
		}
	} else {
		memset(abnd_free_ents, 0, sizeof(abnd_free_ents));
		for (entry = 0; entry < jrange_entry->n_ents; entry++) {
			if (GET_JENTRY_STATE_FROM_BMP(jrange_entry->jentry_state_bmp, entry) == JENTRY_FREE) {
				if ((prev_state = JENTRY_STATE_CHNG(
					jrange_entry, entry, JENTRY_TAKEN, true, chng_ent_reason)) != JENTRY_FREE) {
					_NEs(error_serjio_chk_rng_post_free, serjio_pd, "range @JRNL_RNG_IDX entry @JRNL_RNG_ENT_IDX in unexpected previous state @PREV_STATE",
						jrange_entry->range_idx, entry, prev_state);
					BUG_ON(1);
				}
				set_bit(entry, abnd_free_ents);
			}
		}
		if (!bitmap_empty(abnd_free_ents, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE)) {
			_NTs(trace_3_serjio_chk_rng_post_free, serjio_pd, "Journal Range @JRNL_RNG_IDX N @BINJE (Client @CLIENT_UUID, @CLIENT_ID) - "
				"JAM Abnd2Free " NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
				"(Remaining Abandoned @JENTRY_STATE_CNT Unknown @JENTRY_STATE_CNT)",
				jrange_entry->range_idx, 1 << jrange_entry->binje_shift,
				&jrange_entry->client_uuid, jrange_entry->client_id,
				abnd_free_ents, jrange_entry->jentry_state_cnt[JENTRY_ABND],
				jrange_entry->jentry_state_cnt[JENTRY_UNKNOWN]);
			/* Check to see if we need to update the gen_id */
			if (jrange_entry->ent_gen_id_wrapped) {
				jrange_entry->gen_id++;
				jrange_entry->ent_gen_id_wrapped = false;
				spin_unlock_irqrestore(&jrange_entry->lock, flags);
				/* Write Synchronously so that we don't update JAM before saving the new gen_id */
				if ((rv = write_jrange_to_serjio_db(jrange_entry, NULL, NULL)) < 0) {
					_NEs(chk_rng_post_free_e2, serjio_pd, "write_jrange_to_serjio_db failed (@INT) for range @UINT",
						rv, jrange_entry->range_idx);
					goto out;
				}
			} else
				spin_unlock_irqrestore(&jrange_entry->lock, flags);
			if ((rv = send_jam_abnd_free(serjio_pd, jrange_entry, abnd_free_ents))) {
				if (rv == -ENOENT) {
					_NWs(warn_serjio_chk_rng_post_free, serjio_pd, "client @CLIENT_UUID (@CLIENT_ID) is not found in hash",
						&jrange_entry->client_uuid, jrange_entry->client_id);
					rv = 0;
				} else {
					_NEs(error_1_serjio_chk_rng_post_free, serjio_pd, "send_jam_abnd_free failed (@RV)", rv);
				}
			}
		} else {
			spin_unlock_irqrestore(&jrange_entry->lock, flags);
		}
	}
out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_blkset_recovered(
	struct nvmeibs_disk_info *di, uuid_be client_uuid, u32 range_idx,
	u32 entry_idx, u64 blkset_slba, u64 gen_id)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	int rv = 0;

	(void)gen_id;
	NFIN;
	if (!di->metadata)
		goto out;

	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_blkset_recovered, "Could not get access to SERJIO private data for disk @DISK_ID_STR (@DI)",
		   nvmeibs_disk_info_get_disk_id(di), di);
		rv = -ENODATA;
		goto out;
	}

	_NTs(trace_serjio_nvmeibs_serjio_blkset_recovered, serjio_pd, "DEPRECATED: blkset_recovered (@CLIENT_UUID, r: @JRNL_RNG_IDX, e:@JRNL_RNG_ENT_IDX, slba: @BLKSET_SLBA)",
		&client_uuid, range_idx, entry_idx, blkset_slba);
out:
	NFOUT;
	return rv;
}

static struct jrange_entry* get_jrange_entry_for_uuid(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, uuid_be client_uuid, binje_t binje, bool already_locked)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry* ret_entry = NULL;
	unsigned long flags;

	if (!already_locked)
		spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);

	if (jranges_alloc_tbl->num_reserved_ranges) {
		struct jrange_entry *jrange_iter;
		/* Check if the client has already been assigned a range that it abandoned */
		hash_for_each_possible(jranges_alloc_tbl->reserved_ranges,
							   jrange_iter, link, hash_uuid(client_uuid)) {
			if (nvmeib_uuid_cmp(client_uuid, jrange_iter->client_uuid) == 0) {
				/* Found range already assigned to this client (update client_id) */
				if (binje == NVMEIB_EC_INVALID_JOURNAL_BINJE || (unsigned)(1 << jrange_iter->binje_shift) == binje) {
					_NTs(trace_serjio_get_jrange_entry_for_uuid, serjio_pd,
						 "Found range @JRNL_RNG_IDX with N @BINJE for client @CLIENT_UUID",
						jrange_iter->range_idx, 1 << jrange_iter->binje_shift, &client_uuid);
					ret_entry = jrange_iter;
					break;
				}
				_NTs(trace_serjio_get_jrange_entry_for_uuid_other_n, serjio_pd,
					"Found range @JRNL_RNG_IDX with different N @BINJE for client @CLIENT_UUID."
					" Looking for N @BINJE",
					jrange_iter->range_idx,
					1 << jrange_iter->binje_shift, &client_uuid, binje);
			}
		}
		if (!ret_entry) {
			_NTs(trace_1_serjio_get_jrange_entry_for_uuid, serjio_pd, "No range found for client @CLIENT_UUID", &client_uuid);
		}
	} else {
		_NTs(trace_2_serjio_get_jrange_entry_for_uuid, serjio_pd, "No assigned journal ranges");
	}
	if (!already_locked)
		spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
	return ret_entry;
}

int nvmeibs_serjio_get_client_uuid_rng(struct nvmeibs_disk_info *di, uuid_be client_uuid, binje_t binje)
{
	int rv = -ENOENT;

	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct jrange_entry *jrange_entry;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_get_client_uuid_rng, "SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		_NTs(trace_serjio_nvmeibs_serjio_get_client_uuid_rng, serjio_pd, "Journal is not ready");
		rv = -EAGAIN;
		goto out;
	}

	if ((jrange_entry = get_jrange_entry_for_uuid(serjio_pd, client_uuid, binje, false))) {
		rv = jrange_entry->range_idx;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_clean_journal_for_disk_range(
	struct nvmeibs_disk_info *di, const char *seg_uuid_str, u64 disk_range_start_lba,
	u64 disk_range_end_lba, bool seg_delete)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct cln_jrnl_disk_rng_param param;
	int rv = 0;

	NFIN;
	_NTs(t0_nvmeibs_serjio_clean_journal_for_disk_range, serjio_pd,
		 "start clean-journal of disk=@DISK_ID_STR seg=@STR (seg_delete=@BOOL)",
		 nvmeibs_disk_info_get_disk_id(di), seg_uuid_str, seg_delete);

	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_clean_journal_for_disk_range, "SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}
	if (IS_SERJIO_DYING(serjio_pd)) {
		_NEs(error_1_serjio_nvmeibs_serjio_clean_journal_for_disk_range, serjio_pd, "SERJIO is dying");
		rv = -EBUSY;
		goto out;
	}
	param.seg_uuid_str = seg_uuid_str;
	param.start_lba = disk_range_start_lba;
	param.end_lba = disk_range_end_lba;
	param.seg_delete = seg_delete;

	rv = run_on_io_wq(serjio_pd, io_cln_jrnl_disk_rng_start_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG_START);

out:
	NFOUT;
	return rv;
}

static struct seg_tree_entry *get_j2d_cln_rng(
	struct nvmeibs_serjio_disk_private_data *serjio_pd, u64 j2d, u32 cln_state_mask,
	int rng_idx, bool set_wait_return)
{
	struct seg_tree_entry *seg_tree_iter, *ret = NULL;
	unsigned long flags;
#if KS_RB_ROOT_CACHED
	struct rb_root_cached *iter_root;
#else
	struct rb_root *iter_root;
#endif

	iter_root = &serjio_pd->jrnl_seg_rb_root;
scan_tree:
	for (seg_tree_iter = seg_tree_iter_first(iter_root, j2d, j2d);
		seg_tree_iter != NULL; seg_tree_iter = seg_tree_iter_next(seg_tree_iter, j2d, j2d)) {
		spin_lock_irqsave(&seg_tree_iter->cln_lock, flags);
		if ((1 << seg_tree_iter->cln_state) & cln_state_mask) {
			ret = seg_tree_iter;
			if (set_wait_return) {
				if (!test_and_set_bit(rng_idx, seg_tree_iter->wait_rng_bmp))
					seg_tree_iter->wait_rng_cnt++;
				seg_tree_iter->cln_state = CLN_SEG_WAIT_RNG_RETURN;
			}
			spin_unlock_irqrestore(&seg_tree_iter->cln_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&seg_tree_iter->cln_lock, flags);
	}
	if (iter_root == &serjio_pd->jrnl_seg_rb_root) {
		iter_root = &serjio_pd->del_seg_rb_root;
		goto scan_tree;
	}
	return ret;
}

struct cln_jrnl_disk_rng_cb_param {
	DECLARE_BITMAP(wait_ret_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES);
	DECLARE_BITMAP(cln_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES);
	int n_zero_ent;
	spinlock_t lock;
};

static void cln_jrnl_disk_rng_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	int range_idx = op_rsrc->range_idx;
	int entry_idx = op_rsrc->entry;
	union jblock_md jmdc_entry[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];
	struct seg_tree_entry *seg_tree_entry = NULL;
	unsigned long flags;
	struct jrange_entry *jrng = &jranges_alloc_tbl->ranges[range_idx];
	enum nvmeibs_serjio_jentry_state jentry_state;
	int rv;
	struct cln_jrnl_disk_rng_cb_param *param = op_rsrc->param;
	u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;

	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);
	NFIN;
	if (status != 0) {
		_NEs(error_serjio_cln_jrnl_disk_rng_cb, serjio_pd, "Error (@STATUS) reading journal entry", status);
		goto return_op_rsrc;
	}

	get_nvme_op_rsrc_md(op_rsrc, jmdc_entry, sizeof(jmdc_entry[0]) << jrng->binje_shift);
	if (nvmeib_is_jmd_io_entry(jmdc_entry)) {
		int chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << jrng->binje_shift, &j2d_start, &j2d_end);

		if (chain_err == NVMEIB_JENTRY_CHAIN_OK)
			_NDs(trace_serjio_cln_jrnl_disk_rng_cb, serjio_pd,
				 "Read Journal Metadata (J2D: [@J2D_START,@J2D_END] "
				 "TxID: @TXID TxBmp: @TXBM_COMPRESSED Ver:@X) for "
				 "Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX (@DLBA)",
				j2d_start, j2d_end, jmdc_entry[0].tx_id, jmdc_entry[0].tx_bmp,
				jmdc_entry[0].version, range_idx, entry_idx, (long unsigned)op_rsrc->nvme_req.disk_block);
		else
			_NWs(warn_serjio_cln_jrnl_disk_rng_cb_inv_chain, serjio_pd,
				"Read Invalid Journal Metadata @RAW at @PTR for Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX (@DLBA) Chain Error @ERR_STR Chain Error Block @IDX",
				jmdc_entry[0].raw, jmdc_entry, range_idx, entry_idx, (long unsigned)op_rsrc->nvme_req.disk_block,
				nvmeib_shared_jentry_md_chain_err_str(chain_err), nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
	}

	spin_lock_irqsave(&jrng->lock, flags);
	jentry_state = GET_JENTRY_STATE_FROM_BMP(jrng->jentry_state_bmp, entry_idx);
	spin_unlock_irqrestore(&jrng->lock, flags);
	switch (jentry_state) {
	case JENTRY_FREE:
		/* Nothing to do */
		BUG_ON(!nvmeib_is_jmd_unused_entry(jmdc_entry));
		break;
	case JENTRY_UNKNOWN:
		/* Entry is in UNKNOWN state => Update JMDC */
		set_jmdc_entry_data(jrng, entry_idx, jmdc_entry);
		if (nvmeib_is_jmd_unused_entry(jmdc_entry)) {
			/* Entry has JMDC free value => move to FREE state */
			BUG_ON(JENTRY_STATE_CHNG(jrng, entry_idx, JENTRY_FREE, false, JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_FREE) != JENTRY_UNKNOWN);
			/* Nothing to clean */
			goto return_op_rsrc;
		}
		BUG_ON(!nvmeib_is_jmd_io_entry(jmdc_entry));
		/* Move entry to SYNCED state now that JMDC = JMDD */
		BUG_ON(JENTRY_STATE_CHNG(jrng, entry_idx, JENTRY_SYNCED, false, JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_DIRTY) != JENTRY_UNKNOWN);
		FALLTHRU;
	case JENTRY_SYNCED: {
		if ((seg_tree_entry = get_j2d_cln_rng(
			serjio_pd, j2d_start, (1 << CLN_SEG_SCAN), range_idx, false))) {
			/* BUG_ON if journal entry overflows the segment */
			SERJIO_BUG_ON(j2d_end > seg_tree_entry->elba, err_serjio_cln_jrnl_disk_rng_cb_synced, serjio_pd,
				      "@JRANGE_STATUS Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX with J2D "
							"[@J2D_START,@J2D_END] overflows segment @SEG_UUID_STR with LBA Range [@SLBA_LLONG, @ELBA]",
							jrng->status, range_idx, entry_idx, j2d_start, j2d_end,
							seg_tree_entry->seg_uuid_str, seg_tree_entry->slba, seg_tree_entry->elba);
			_NTs(trace_1_serjio_cln_jrnl_disk_rng_cb, serjio_pd, "@JRANGE_STATUS Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX has J2D [@J2D_START,@J2D_END] in LBA Range [@SLBA_LLONG, @ELBA] of Seg @SEG_UUID_STR. Cleaning",
				jrng->status, range_idx, entry_idx, j2d_start, j2d_end,
				seg_tree_entry->slba, seg_tree_entry->elba, seg_tree_entry->seg_uuid_str);
			/* For SYNCED entries, we can zero them as they are in our control. */
			param->n_zero_ent++;
			set_bit(range_idx, param->cln_rng_bmp);
			if ((rv = zero_journal_entry_from_cb(op_rsrc, NULL, NULL, NVME_OP_CB,
				"%s - rng %u N %u ent %u j2d_start %llu j2d_end %llu in seg %s\n",
				__func__, range_idx, jrng->binje_shift, entry_idx, j2d_start, j2d_end, seg_tree_entry->seg_uuid_str))) {
				_NEs(error_1_serjio_cln_jrnl_disk_rng_cb, serjio_pd, "Error @RV zeroing journal entry", rv);
				goto return_op_rsrc;
			}
			goto out;
		}
		break;
	}
	case JENTRY_TAKEN:
	case JENTRY_WAIT_RET: {
		/* Check to see if the TAKEN J2D points to any pending clean operations */
		if ((seg_tree_entry = get_j2d_cln_rng(
			serjio_pd, j2d_start, (1 << CLN_SEG_SCAN), range_idx, true))) {
			SERJIO_BUG_ON(j2d_end > seg_tree_entry->elba, err_serjio_cln_jrnl_disk_rng_cb_taken, serjio_pd,
				      "@JRANGE_STATUS Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX with J2D "
					"[@J2D_START,@J2D_END] overflows segment @SEG_UUID_STR with LBA Range [@SLBA_LLONG, @ELBA]",
					jrng->status, range_idx, entry_idx, j2d_start, j2d_end,
					seg_tree_entry->seg_uuid_str, seg_tree_entry->slba, seg_tree_entry->elba);
			/* For TAKEN entries, we move them to WAIT_RETURN and later disconnect the client */
			JENTRY_STATE_CHNG(jrng, entry_idx, JENTRY_WAIT_RET, false,
					  JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_IN_SEG);
			/* Set the param bit that we are waiting for the range to return */
			spin_lock_irqsave(&param->lock, flags);
			set_bit(range_idx, param->wait_ret_rng_bmp);
			spin_unlock_irqrestore(&param->lock, flags);
		}
		break;
	}
	case JENTRY_IO_ERR:
		/* Entry has IO Error - Nothing to do */
		break;
	default:
		BUG();
	}

return_op_rsrc:
	/* This will also signal the completion that the io thread is waiting for */
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(serjio_pd, op_rsrc, NVME_OP_CB);
out:
	NFOUT;
}

static DECLARE_IO_WQ_FN(io_cln_jrnl_disk_rng_start_fn)
{
	int rv = 0;
	struct cln_jrnl_disk_rng_param *cln_rng_param = param;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	unsigned long flags;
	struct seg_tree_entry *seg_tree_entry = NULL, *seg_tree_iter;
	const char *seg_uuid_str = cln_rng_param->seg_uuid_str;
	u64 start_lba = cln_rng_param->start_lba;
	u64 end_lba = cln_rng_param->end_lba;
	bool seg_delete = cln_rng_param->seg_delete;

	NFIN;
	if (!SERJIO_RUNNING(srj_state)) {
		if (SERJIO_BOOTING(srj_state)) {
			_NTs(io_cln_jrnl_disk_rng_start_fn_t1, serjio_pd, "SERJIO in booting state @SERJIO_STATE - Rescheduling Clean Segment", srj_state);
			*resched = true;
			rv = -EAGAIN;
		}
		else if (SERJIO_HALTING(srj_state)) {
		_NTs(io_cln_jrnl_disk_rng_start_fn_t2, serjio_pd, "SERJIO in shutdown state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
		} else {
			_NTs(io_cln_jrnl_disk_rng_start_fn_t3, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
			rv = -EINVAL;
			BUG_ON(1);
		}
		goto out;
	}

	/* Find the segment using the ID */
	hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_tree_iter, link, hash_uuid_str(seg_uuid_str)) {
		if (strncmp(seg_tree_iter->seg_uuid_str, seg_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
			seg_tree_entry = seg_tree_iter;
			break;
		}
	}
	if (!seg_tree_entry) {
		hash_for_each_possible(serjio_pd->del_seg_tbl, seg_tree_iter, link, hash_uuid_str(seg_uuid_str)) {
			if (strncmp(seg_tree_iter->seg_uuid_str, seg_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
				BUG_ON(!seg_tree_iter->seg_delete);
				seg_tree_entry = seg_tree_iter;
				break;
			}
		}
	}

	if (!seg_tree_entry) {
		_NWs(trace_1_serjio_io_cln_jrnl_disk_rng_start_fn, serjio_pd, "Segment @SEG_UUID_STR not found. Reporting to TOMA as cleaned", seg_uuid_str);
		if ((rv = nvmeibs_toma_report_event_serjio_disk_range_cleaned(
			serjio_pd->di, seg_uuid_str)))
		{
			_NEs(error_serjio_io_cln_jrnl_disk_rng_start_fn_failed_report, serjio_pd, 
			     "Failed (@INT) to report segment @STR clean to TOMA",
			     rv, seg_uuid_str);
		}
		goto out;
	}

	if (seg_tree_entry->slba != start_lba || seg_tree_entry->elba != end_lba) {
		_NTs(trace_2_serjio_io_cln_jrnl_disk_rng_start_fn, serjio_pd, "Partial clean of segment @SEG_UUID_STR not supported", seg_uuid_str);
		rv = -ENOTSUPP;
		goto out;
	}

	spin_lock_irqsave(&seg_tree_entry->cln_lock, flags);
	if (seg_tree_entry->cln_state == CLN_SEG_DONE) {
		/* [NVMESH-4700]: If TOMA is restarted at the wrong moment, it might request another clean of a deleted segment. Easiest solution is to just do it again */
		_NTs(io_cln_jrnl_disk_rng_start_fn_cln_seg_done, serjio_pd,
		     "TOMA requested another clean of already cleaned segment @SEG_UUID_STR (deleted @BOOL_YN)."
		     "Resetting state to Idle", seg_tree_entry->seg_uuid_str, seg_tree_entry->seg_delete);
		seg_tree_entry->cln_state = CLN_SEG_IDLE;
	}
	if (seg_tree_entry->cln_state != CLN_SEG_IDLE) {
		spin_unlock_irqrestore(&seg_tree_entry->cln_lock, flags);
		_NTs(io_cln_jrnl_disk_rng_start_fn_cln_seg_not_idle, serjio_pd,
		     "Clean of segment @SEG_UUID_STR already in progress (state @CLN_SEG_STATE, deleted @BOOL_YN)",
		     seg_tree_entry->seg_uuid_str, seg_tree_entry->cln_state, seg_tree_entry->seg_delete);
		rv = -EALREADY;
		goto out;
	}

	BUG_ON(seg_tree_entry->wait_rng_cnt != 0);
	BUG_ON(!bitmap_empty(seg_tree_entry->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
	BUG_ON(!list_empty(&seg_tree_entry->cln_link));
	seg_tree_entry->cln_state = CLN_SEG_WAIT_SCAN;
	seg_tree_entry->seg_delete = seg_delete;
	spin_unlock_irqrestore(&seg_tree_entry->cln_lock, flags);

	list_add_tail(&seg_tree_entry->cln_link, &serjio_pd->cln_seg_wait_scan_list);

	_NTs(trace_4_serjio_io_cln_jrnl_disk_rng_start_fn, serjio_pd, "Scheduling clean of @YES_NO_STATUS segment @SEG_UUID_STR",
		seg_tree_entry->seg_delete ? "deleted" : "",
		seg_tree_entry->seg_uuid_str);

	/* Schedule the clean */
	rv = run_on_io_wq(serjio_pd, io_cln_jrnl_disk_rng_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_CLN_JRNL_DISK_RNG);
out:
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_cln_jrnl_disk_rng_fn)
{
	int rv = 0, toma_rv;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *jrange;
	struct seg_tree_entry *seg_tree_entry;
	unsigned entry;
	u32 i;
	enum nvmeibs_serjio_state srj_state;
	enum nvmeibs_serjio_state state_err = SERJIO_ERR_GENERAL;
	atomic_t ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(comp);
	unsigned long flags;
	enum nvmeibs_serjio_jentry_state jentry_state;
	struct cln_jrnl_disk_rng_cb_param *cb_param = NULL;
	int n_zero_ent = 0, tot_zero_ent = 0;
	DECLARE_BITMAP(wait_rnjs_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES) = {0};

	NFIN;
	(void)param;
	if ((srj_state = serjio_state_get(serjio_pd)) != SERJIO_READY) {
		if (SERJIO_HALTING(srj_state)) {
		_NTs(io_cln_jrnl_disk_rng_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
		} else if (SERJIO_RUNNING(srj_state)) {
			_NTs(io_cln_jrnl_disk_rng_fn_t2, serjio_pd, "SERJIO in running state @SERJIO_STATE - Rescheduling Clean Journal", srj_state);
			*resched = true;
			rv = -EAGAIN;
		} else if (SERJIO_BOOTING(srj_state)) {
			_NTs(io_cln_jrnl_disk_rng_fn_t3, serjio_pd, "SERJIO in booting state @SERJIO_STATE", srj_state);
			rv = -EBUSY;
		} else {
			_NTs(io_cln_jrnl_disk_rng_fn_t4, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
			BUG_ON(1);
		}
		goto out;
	}

	if (!(cb_param = kzalloc(sizeof(*cb_param), GFP_KERNEL))) {
		_NEs(io_cln_jrnl_disk_rng_fn_e1, serjio_pd, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}

	/* Move all entries in wait list to scan list */
	while ((seg_tree_entry = list_first_entry_or_null(
			&serjio_pd->cln_seg_wait_scan_list, struct seg_tree_entry, cln_link))) {
		spin_lock_irqsave(&seg_tree_entry->cln_lock, flags);
		BUG_ON(seg_tree_entry->cln_state != CLN_SEG_WAIT_SCAN);
		seg_tree_entry->cln_state = CLN_SEG_SCAN;
		spin_unlock_irqrestore(&seg_tree_entry->cln_lock, flags);
		list_del(&seg_tree_entry->cln_link);
		list_add_tail(&seg_tree_entry->cln_link, &serjio_pd->cln_seg_scan_list);
		_NTs(io_cln_jrnl_disk_rng_fn_t5, serjio_pd, "scanning journal for segment @STR [@INT_ULLONG, @INT_ULLONG]",
			seg_tree_entry->seg_uuid_str, seg_tree_entry->slba, seg_tree_entry->elba);
	}

	if (list_empty(&serjio_pd->cln_seg_scan_list)) {
		/* This can happen if the list was emptied by a previous iteration */
		_NTs(trace_1_serjio_io_cln_jrnl_disk_rng_fn, serjio_pd, "nothing to do");
		goto out;
	}

	/* Scan the JMDC/JMDD to see which entries need cleaning. For entries that are:
	 * - FREE/ABANDONED/SYNCED, the JMDC == JMDD so we can just look at the JMDC
	 * - TAKEN/UNKNOWN, the JMDC may not equal the JMDD so we need to read it.
	 * - UNKNOWN, we can also update the JMDC and move the state to SYNCED
	 * when we read it
	 */

	/* First lets scan the JMDC for the FREE/ABANDONED/SYNCED entries */
	for (i = 0, jrange = serjio_pd->jranges_alloc_tbl.ranges;
		 i < serjio_pd->jranges_alloc_tbl.num_ranges; i++, jrange++) {
		spin_lock_irqsave(&jrange->lock, flags);
		if (jrange->status != JRANGE_RESERVED && jrange->status != JRANGE_ALLOCATED)
			goto next_rng;
		for (entry = 0; entry < jrange->n_ents; entry++) {
			const union jblock_md *jmdc_entry = get_jmdc_entry(jrange, entry);
			jentry_state = GET_JENTRY_STATE_FROM_BMP(jrange->jentry_state_bmp, entry);
			if (JENTRY_OWNED_BY_SERJIO(jentry_state)) {
				u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
				u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
				int chain_err;
				if (nvmeib_is_jmd_unused_entry(jmdc_entry))
					continue;
				if ((chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_entry, 1 << jrange->binje_shift, &j2d_start, &j2d_end)) != NVMEIB_JENTRY_CHAIN_OK) {
					_NWs(warn_serjio_io_cln_jrnl_disk_rng_fn_inv_chain, serjio_pd,
							"Journal Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX has invalid Chain Raw: @RAW Ptr: @PTR Chain Error @ERR_STR Chain Error Block @IDX",
							jrange->range_idx, 1 << jrange->binje_shift, entry, jmdc_entry[0].raw, jmdc_entry,
							nvmeib_shared_jentry_md_chain_err_str(chain_err), nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
					continue;
				}
				if (!(seg_tree_entry = get_j2d_cln_rng(serjio_pd, j2d_start,
					(1 << CLN_SEG_SCAN), jrange->range_idx, false)))
					continue;
				SERJIO_BUG_ON(j2d_end > seg_tree_entry->elba,
							err_serjio_io_cln_jrnl_disk_rng_fn_j2d_overflow, serjio_pd,
							"@JRANGE_STATUS Journal Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX "
							" J2D [@J2D_START,@J2D_END] overflows Segment @SEG_UUID_STR  [@SLBA_LLONG, @ELBA]",
							jrange->status, jrange->range_idx, 1 << jrange->binje_shift, entry,
							j2d_start, j2d_end, seg_tree_entry->seg_uuid_str, seg_tree_entry->slba,
							seg_tree_entry->elba);
				_NDs(trace_2_serjio_io_cln_jrnl_disk_rng_fn, serjio_pd, "@JRANGE_STATUS Journal Range @JRNL_RNG_IDX N @BINJE Entry @JRNL_RNG_ENT_IDX has J2D [@J2D_START,@J2D_END] in LBA Range"
							" [@SLBA_LLONG, @ELBA] of Segment @SEG_UUID_STR being @YES_NO_STATUS",
							jrange->status, jrange->range_idx, 1 << jrange->binje_shift, entry,
							j2d_start, j2d_end, seg_tree_entry->slba,
							seg_tree_entry->elba, seg_tree_entry->seg_uuid_str,
							seg_tree_entry->seg_delete ? "Deleted" : "Cleaned");
				n_zero_ent++;
				spin_unlock_irqrestore(&jrange->lock, flags);
				/* Entry points at the segment => Zero it */
				if ((rv = zero_journal_entry(serjio_pd, jrange, entry, &ctr, &comp, NULL, NULL,
				"%s - rng: %u binje: %u ent: %u state: %s j2d [%llu,%llu] points to %s seg %s\n",
				__func__, jrange->range_idx, 1 << jrange->binje_shift, entry, nvmeib_shared_serjio_jentry_state_to_str(jentry_state),
				j2d_start, j2d_end, seg_tree_entry->seg_delete ? "deleted" : "",
				seg_tree_entry->seg_uuid_str))) {
					if (atomic_dec_return(&ctr) > 0)
						wait_for_completion(&comp);
					_NEs(error_serjio_io_cln_jrnl_disk_rng_fn, serjio_pd, "zero_journal_entry failed (@RV) for range @JRNL_RNG_IDX entry @JRNL_RNG_ENT_IDX",
						rv, jrange->range_idx, entry);
					rv = -EIO;
					state_err = SERJIO_ERR_WR_JRNL;
					goto out;
				}
				spin_lock_irqsave(&jrange->lock, flags);
			}
		}
next_rng:
		spin_unlock_irqrestore(&jrange->lock, flags);

		if (n_zero_ent) {
			_NTs(trace_serjio_io_cln_jrnl_disk_rng_fn_zero_ents, serjio_pd,
				"Issued @NUM_ENTS zero-jentries for range @JRNL_RNG_IDX, "
				"adding to wait-bmp\n",
				n_zero_ent, jrange->range_idx);
			set_bit(jrange->range_idx, wait_rnjs_bmp);
			tot_zero_ent += n_zero_ent;
			n_zero_ent = 0;
		}
	}

	_NTs(t0_serjio_io_cln_jrnl_disk_rng_fn_zero_ents, serjio_pd,
		"Issued total of @INT zero-jentries for all ranges, "
		"wait for remaining @INT to complete\n",
		tot_zero_ent, atomic_read(&ctr));
	if (atomic_dec_return(&ctr) > 0)
		wait_for_completion(&comp);

	atomic_set(&ctr, 1);
	reinit_completion(&comp);
	for_each_set_bit(i, wait_rnjs_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES) {
		_NTs(t1_serjio_io_cln_jrnl_disk_rng_fn_zero_ents, serjio_pd,
			"Run post-free actions for @JRNL_RNG_IDX\n", i);
		jrange = &jranges_alloc_tbl->ranges[i];
		sync_jmdc_range_to_all_nics(serjio_pd, jrange->range_idx);
		chk_rng_post_free(jrange, &comp, &ctr, JENTRY_STATE_CHNG_REASON_CLEAN_SEG);
	}

	_NTs(t2_serjio_io_cln_jrnl_disk_rng_fn_zero_ents, serjio_pd,
		"Wait for @INT disk-writes from post-free actions for all ranges",
		 atomic_read(&ctr));
	if (atomic_dec_return(&ctr) > 0)
		wait_for_completion(&comp);

	/* Now read the unsynced entries from the disk */
	if ((rv = rd_jrnl(serjio_pd, cln_jrnl_disk_rng_cb, cb_param,
		SERJIO_READY, false, JENTRY_UNSYNCED_MASK))) {
		_NEs(error_1_serjio_io_cln_jrnl_disk_rng_fn, serjio_pd, "rd_jrnl failed (@RV)", rv);
		state_err = SERJIO_ERR_RD_JRNL;
		goto out;
	}

	if (cb_param->n_zero_ent) {
		_NTs(trace_io_cln_jrnl_disk_rng_fn_zeroed_unsynced,
			serjio_pd, "Zeroed @N_ENTS entries\n", cb_param->n_zero_ent);
	}

	/* Check all ranges we cleaned entries from */
	atomic_set(&ctr, 1);
	reinit_completion(&comp);
	for_each_set_bit(i, cb_param->cln_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES) {
		jrange = &jranges_alloc_tbl->ranges[i];
		sync_jmdc_range_to_all_nics(serjio_pd, jrange->range_idx);
		chk_rng_post_free(jrange, &comp, &ctr, JENTRY_STATE_CHNG_REASON_CLEAN_SEG);
	}

	if (atomic_dec_return(&ctr) > 0)
		wait_for_completion(&comp);

	/* See which segments are waiting for entries to return and which can be completed */
	while ((seg_tree_entry = list_first_entry_or_null(
			&serjio_pd->cln_seg_scan_list, struct seg_tree_entry, cln_link))) {
		spin_lock_irqsave(&seg_tree_entry->cln_lock, flags);
		if (seg_tree_entry->cln_state == CLN_SEG_SCAN) {
			/* Still in SCAN state => Complete to TOMA */
			BUG_ON(seg_tree_entry->wait_rng_cnt != 0);
			BUG_ON(!bitmap_empty(seg_tree_entry->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
			seg_tree_entry->cln_state = seg_tree_entry->seg_delete ? CLN_SEG_DONE : CLN_SEG_IDLE;
			spin_unlock_irqrestore(&seg_tree_entry->cln_lock, flags);
			list_del_init(&seg_tree_entry->cln_link);
			if (seg_tree_entry->seg_delete && seg_tree_entry->gpt_idx == -1) {
				/* Segment is not in the GPT, so we can simply remove it from the interval tree and free it */
				_NTs(io_cln_jrnl_disk_rng_fn_t6, serjio_pd, "Finished cleaning deleted segment @STR", seg_tree_entry->seg_uuid_str);
				hash_del(&seg_tree_entry->link);
				seg_tree_remove(seg_tree_entry, &serjio_pd->del_seg_rb_root);
				kfree(seg_tree_entry);
			} else {
				_NTs(io_cln_jrnl_disk_rng_fn_t7, serjio_pd, "Finished cleaning segment @STR. Reporting to TOMA", seg_tree_entry->seg_uuid_str);
				if ((toma_rv = nvmeibs_toma_report_event_serjio_disk_range_cleaned(
					serjio_pd->di, seg_tree_entry->seg_uuid_str))) {
					_NEs(io_cln_jrnl_disk_rng_fn_e2, serjio_pd, "Failed (@INT) to report segment @STR clean to TOMA",
						toma_rv, seg_tree_entry->seg_uuid_str);
				}
			}
		} else {
			BUG_ON(seg_tree_entry->cln_state != CLN_SEG_WAIT_RNG_RETURN);
			BUG_ON(seg_tree_entry->wait_rng_cnt == 0);
			BUG_ON(bitmap_empty(seg_tree_entry->wait_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES));
			spin_unlock_irqrestore(&seg_tree_entry->cln_lock, flags);
			list_del(&seg_tree_entry->cln_link);
			list_add_tail(&seg_tree_entry->cln_link, &serjio_pd->cln_seg_wait_ret_list);
		}
	}

	if (!bitmap_empty(cb_param->wait_ret_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES)) {
		/* Change state to prevent new allocations */
		BUG_ON(serjio_state_cmp_exch(
			serjio_pd, SERJIO_READY, SERJIO_CLN_JRNL) != SERJIO_READY);

		/* Disconnect all clients with entries that we are waiting to return */
		for_each_set_bit(i, cb_param->wait_ret_rng_bmp, NVMEIB_EC_MAX_JOURNAL_RANGES) {
			jrange = &jranges_alloc_tbl->ranges[i];

		_NTs(trace_3_serjio_io_cln_jrnl_disk_rng_fn, serjio_pd, "Range @JRNL_RNG_IDX needs to return to check entries. "
		"Releasing Client @CLIENT_UUID (@CID_LLONG, @CLIENT_HOST)",
			jrange->range_idx, &jrange->client_uuid, jrange->client_id, jrange->client_host);
			if ((rv = nvmeibs_remove_cid_clients(jrange->client_id,
					 NVMEIBS_LOGOUT_REASON_JOURNAL_CLEAR) < 0)) {
				_NEs(error_serjio_6141, serjio_pd, "nvmeibs_remove_cid_clients failed (@RV)\n", rv);
			}
		}
	}

out:
	kfree(cb_param);
	if (rv) {
		if (rv != -ECANCELED && rv != -EAGAIN && rv != EBUSY) {
			/* Clean Failed - Put SERJIO in Error State */
			_NWs(io_cln_jrnl_disk_rng_fn_w1, serjio_pd, "clean failed (@INT) => ERROR", rv);
			serjio_go_to_err_state(serjio_pd, state_err);
		}
	}

	NFOUT;
	return rv;
}

static DECLARE_SERJIO_RNG_CB_GET_JMDC_ENT_FN(call_for_range_get_jmdc_ent_fn)
{
	return get_jmdc_entry(rng_handle, rng_entry);
}

static int call_for_range(struct jrange_entry *jrange_entry, u64 start_lba, u64 end_lba,
						  nvmeibs_serjio_rng_cb_fn cb_fn, void *ctx)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = jrange_entry->serjio_pd;
	DECLARE_BITMAP(dirty_ents_in_seg_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	DECLARE_BITMAP(abnd_ents_in_seg_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	int n_dirty_ents_in_seg;
	int n_abnd_ents_in_seg;
	unsigned long flags;
	int rv;
	unsigned entry;
	u32 range_idx = jrange_entry->range_idx;
	u32 jmdc_size = (jrange_entry->n_ents << jrange_entry->binje_shift) *
		sizeof(union jblock_md);

	NFIN;

	_NTs(t0_serjio_call_for_rng, serjio_pd,
		 "Prep calling '@FN' for range @JRNL_RNG_IDX", cb_fn, range_idx);

	/* Calculate dirty_ents_in_seg_bmp */
	memset(dirty_ents_in_seg_bmp, 0, sizeof(dirty_ents_in_seg_bmp));
	memset(abnd_ents_in_seg_bmp, 0, sizeof(abnd_ents_in_seg_bmp));
	n_dirty_ents_in_seg = 0;
	n_abnd_ents_in_seg = 0;
	spin_lock_irqsave(&jrange_entry->lock, flags);
	for (entry = 0; entry < jrange_entry->n_ents; entry++) {
		const enum nvmeibs_serjio_jentry_state cur_jentry_state = GET_JENTRY_STATE_FROM_BMP(jrange_entry->jentry_state_bmp, entry);
		if (cur_jentry_state == JENTRY_ABND) {
			set_bit(entry, abnd_ents_in_seg_bmp);
			n_abnd_ents_in_seg++;
		}
		if (JENTRY_DIRTY(cur_jentry_state)) {
			union jblock_md *jmdc_ent = get_jmdc_entry(jrange_entry, entry);
			u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
			u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
			int chain_err = nvmeibs_serjio_jmd_decode_j2d_chain(jmdc_ent, 1 << jrange_entry->binje_shift, &j2d_start, &j2d_end);

			if (chain_err != NVMEIB_JENTRY_CHAIN_OK) {
				_NWs(serjio_call_for_rng_inv_chain, serjio_pd,
				"Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX has invalid chain at @PTR."
				" Chain Error @ERR_STR Chain Error Block @IDX",
				jrange_entry->range_idx, entry, jmdc_ent,
				nvmeib_shared_jentry_md_chain_err_str(chain_err),
				nvmeib_shared_jentry_md_chain_err_block_idx(chain_err));
				continue;
			}
			if ((j2d_start >= start_lba) && (j2d_end <= end_lba)) {
				set_bit(entry, dirty_ents_in_seg_bmp);
				n_dirty_ents_in_seg++;
			}
			continue;
		}
	}
	spin_unlock_irqrestore(&jrange_entry->lock, flags);

	/* write_get_jmdc
	   gen_get_jmdc */
	_NDs(trace_call_for_range_dbg, serjio_pd,
		"@FN(@JRNL_RNG_IDX, @BINJE, @COUNT, @CLIENT_UUID, @START_CLSECT, @SIZE_CLSECT,"
		"@JRNL_RNG_GEN_ID, @NUM_ENTS, " NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE ", @NUM_ENTS, " NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE ",...",
		cb_fn, range_idx, 1 << jrange_entry->binje_shift, jrange_entry->n_ents, &jrange_entry->client_uuid,
		JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, jrange_entry->range_idx, 0),
		JOURNAL_ENTS_TO_NVMEIBC_SECTS(jrange_entry->binje_shift, jrange_entry->n_ents),
		jrange_entry->gen_id, n_dirty_ents_in_seg, dirty_ents_in_seg_bmp,
		n_abnd_ents_in_seg, abnd_ents_in_seg_bmp);
	_NDs(trace_call_for_range_dbg_2, serjio_pd,
		"...@JMDC, @JMDC_LEN, @PTR, @SIZE, @PTR)",
		get_jmdc_entry(jrange_entry, 0), jmdc_size, jrange_entry->jentry_md,
		NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE * sizeof(*jrange_entry->jentry_md), ctx);

	rv = (*cb_fn)(range_idx, 1 << jrange_entry->binje_shift,
			jrange_entry->n_ents, jrange_entry->client_uuid,
			DISK_LBAS_TO_NVMEIBC_SECTS(serjio_pd->di, serjio_pd->disk_ranges.journal.lba + jrange_entry->rng_rlba),
			DISK_LBAS_TO_NVMEIBC_SECTS(serjio_pd->di, jrange_entry->rng_nlba),
			jrange_entry->gen_id, n_dirty_ents_in_seg, dirty_ents_in_seg_bmp,
			n_abnd_ents_in_seg, abnd_ents_in_seg_bmp,
			jrange_entry, call_for_range_get_jmdc_ent_fn,
			jrange_entry->jentry_md, jrange_entry->n_ents * sizeof(*jrange_entry->jentry_md), ctx);
	_NTs(trace_call_for_range, serjio_pd, "@FN, rv=@RV", cb_fn, rv);

	/* EC-7157:
	   if we return -EAGAIN to work-func called from run_on_io_wq_workfn(), the
	   later will add the work-func to pending list, and if there is no upcoming
	   serjio's state-change, the work will never resume and will stuck the ctx
	   which added it */
	WARN_ON(rv == -EAGAIN);

	NFOUT;
	return rv;
}

struct call_for_each_assgn_rng_param {
	const char *seg_uuid;
	u32 start_rng;
	u32 num_rng;
	nvmeibs_serjio_rng_cb_fn cb_fn;
	void *ctx;
};

static DECLARE_IO_WQ_FN(io_call_assgn_rng_fn)
{
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	struct call_for_each_assgn_rng_param *call_rng_param = param;
	struct jrange_entry* jrange_entry;
	int rv = 0;
	const char *seg_uuid = call_rng_param->seg_uuid;
	u32 start_rng = call_rng_param->start_rng;
	u32 num_rng = call_rng_param->num_rng;
	u32 range_idx;
	nvmeibs_serjio_rng_cb_fn cb_fn = call_rng_param->cb_fn;
	void *ctx = call_rng_param->ctx;
	u64 start_lba, end_lba;
	struct seg_tree_entry *seg_rb_iter, *seg_tree_entry = NULL;

	NFIN;

	_NTs(t0_serjio_io_call_assgn_rng_fn, serjio_pd,
		 "start_rng=@INT, num_rng=@INT", start_rng, num_rng);

	if (srj_state != SERJIO_READY && srj_state != SERJIO_GPT_UPDATE && srj_state != SERJIO_CLN_JRNL) {
		_NTs(trace_serjio_io_call_assgn_rng_fn, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
		rv = -ECANCELED;
		goto out;
	}

	if (!seg_uuid || strlen(seg_uuid) == 0) {
		start_lba = 0;
		end_lba = nvmeibs_disk_info_get_num_blks(serjio_pd->di, false) - 1;
	} else {
		/* Find the segment using the id */
		hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_rb_iter, link, hash_uuid_str(seg_uuid)) {
			if (strncmp(seg_rb_iter->seg_uuid_str, seg_uuid, NVMEIB_GID_STR_MAX) == 0) {
				seg_tree_entry = seg_rb_iter;
				start_lba = seg_tree_entry->slba;
				end_lba = seg_tree_entry->elba;
				break;
			}
		}
		if (!seg_tree_entry) {
			_NWs(warn_serjio_nvmeibs_serjio_io_call_assgn_rng_fn, serjio_pd,
				 "Segment @SEG_UUID not found", seg_uuid);
			rv = -ENOENT;
			goto out;
		}
	}

	if (!num_rng)
		num_rng = jranges_alloc_tbl->num_ranges - start_rng;

	for (range_idx = start_rng; range_idx <
			min(start_rng + num_rng, jranges_alloc_tbl->num_ranges); range_idx++) {
		jrange_entry = &jranges_alloc_tbl->ranges[range_idx];
		if (jrange_entry->status != JRANGE_RESERVED && jrange_entry->status != JRANGE_ALLOCATED) {
			continue;
		}
		/* write_get_jmdc
		   gen_get_jmdc */
		if ((rv = call_for_range(jrange_entry, start_lba, end_lba, cb_fn, ctx)) < 0)
			goto out;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_call_for_each_assigned_range(
	struct nvmeibs_disk_info *di, u32 start_range_idx, u32 num_ranges,
	const char *seg_uuid, nvmeibs_serjio_rng_cb_fn cb_fn, void *ctx)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct call_for_each_assgn_rng_param param = {
		.seg_uuid = seg_uuid,
		.start_rng = start_range_idx,
		.num_rng = num_ranges,
		.cb_fn = cb_fn,
		.ctx = ctx,
	};
	int rv;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_call_for_each_assigned_range, "SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		_NTs(trace_serjio_nvmeibs_serjio_call_for_each_assigned_range, serjio_pd, "Journal is not ready");
		rv = -EAGAIN;
		goto out;
	}

	rv = run_on_io_wq(serjio_pd, io_call_assgn_rng_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_CALL_ASSGN_RNG);

out:
	NFOUT;
	return rv;
}

struct call_for_client_range_param {
	uuid_be client_uuid;
	binje_t binje;
	const char *seg_uuid;
	nvmeibs_serjio_rng_cb_fn cb_fn;
	void *ctx;
};

static DECLARE_IO_WQ_FN(io_call_rng_fn)
{
	struct jrange_entry* jrange_entry;
	int rv = 0;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	struct call_for_client_range_param *call_rng_param = param;
	uuid_be client_uuid = call_rng_param->client_uuid;
	const char *seg_uuid = call_rng_param->seg_uuid;
	binje_t binje = call_rng_param->binje;
	nvmeibs_serjio_rng_cb_fn cb_fn = call_rng_param->cb_fn;
	void *ctx = call_rng_param->ctx;
	u64 start_lba, end_lba;
	struct seg_tree_entry *seg_rb_iter, *seg_tree_entry = NULL;

	NFIN;
	if (srj_state != SERJIO_READY && srj_state != SERJIO_GPT_UPDATE && srj_state != SERJIO_CLN_JRNL) {
		_NTs(trace_serjio_io_call_rng_fn, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
		rv = -ECANCELED;
		goto out;
	}

	if (!seg_uuid || strlen(seg_uuid) == 0) {
		start_lba = 0;
		end_lba = nvmeibs_disk_info_get_num_blks(serjio_pd->di, false) - 1;
	} else {
		/* Find the segment using the id */
		hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_rb_iter, link, hash_uuid_str(seg_uuid)) {
			if (strncmp(seg_rb_iter->seg_uuid_str, seg_uuid, NVMEIB_GID_STR_MAX) == 0) {
				seg_tree_entry = seg_rb_iter;
				start_lba = seg_tree_entry->slba;
				end_lba = seg_tree_entry->elba;
				break;
			}
		}
		if (!seg_tree_entry) {
			_NWs(warn_serjio_nvmeibs_serjio_io_call_rng_fn, serjio_pd,
				 "Segment @SEG_UUID not found", seg_uuid);
			rv = -ENOENT;
			goto out;
		}
	}

	if (!(jrange_entry = get_jrange_entry_for_uuid(serjio_pd, client_uuid, binje, false))) {
		_NTs(trace_serjio_io_call_rng_fn_not_found, serjio_pd,
			"Client @CLIENT_UUID has no range allocated", &client_uuid);
		rv = -ENOENT;
		goto out;
	}

	_NTs(trace_serjio_io_call_rng_fn_rng_found, serjio_pd,
		"Range @JRNL_RNG_IDX found for @CLIENT_UUID",
		jrange_entry->range_idx, &client_uuid);

	rv = call_for_range(jrange_entry, start_lba, end_lba, cb_fn, ctx);

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_call_for_client_range(
	struct nvmeibs_disk_info *di, uuid_be client_uuid, binje_t binje,
	const char *seg_uuid, nvmeibs_serjio_rng_cb_fn cb_fn, void *ctx)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct call_for_client_range_param param = {
		.client_uuid = client_uuid,
		.binje = binje,
		.seg_uuid = seg_uuid,
		.cb_fn = cb_fn,
		.ctx = ctx,
	};
	int rv;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_call_for_client_range_no_serjio_pd,
			"SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}

	if (!IS_SERJIO_READY(serjio_pd)) {
		_NTs(trace_serjio_nvmeibs_serjio_call_for_client_range_not_ready, serjio_pd,
			 "Journal is not ready");
		rv = -EAGAIN;
		goto out;
	}

	rv = run_on_io_wq(serjio_pd, io_call_rng_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_CALL_RNG);

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_get_jrnl_clsects(struct nvmeibs_disk_info *di,
								 u64 *jrnl_start_clsect, u64 *jrnl_len_clsect)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	int rv = 0;

	NFIN;
	if (!serjio_pd) {
		_NE(error_serjio_nvmeibs_serjio_get_jrnl_clsects, "SERJIO: Disk has no serjio private data");
		rv = -ENODATA;
		goto out;
	}

	*jrnl_start_clsect =
		DISK_LBAS_TO_NVMEIBC_SECTS(di, serjio_pd->disk_ranges.journal.lba);
	*jrnl_len_clsect =
		DISK_LBAS_TO_NVMEIBC_SECTS(di, serjio_pd->disk_ranges.journal.len_nlbas);

out:
	NFOUT;
	return rv;
}

struct io_free_jrnl_ents_param
{
	const char *serjio_boot_id;
	const char *seg_uuid;
	enum nvmeib_recov_src src;
	u64 blkset_slba;
	int num_ents;
	const struct nvmeib_free_ents_data *free_ents;
};

static DECLARE_IO_WQ_FN(io_free_jrnl_ents_fn)
{
	unsigned long flags;
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *rng = NULL;
	int rv = 0;
	struct io_free_jrnl_ents_param *fn_param = param;
	const char *serjio_boot_id = fn_param->serjio_boot_id;
	const char *seg_uuid = fn_param->seg_uuid;
	u64 blkset_slba = fn_param->blkset_slba;
	int num_ents = fn_param->num_ents;
	const struct nvmeib_free_ents_data *free_ents;
	enum nvmeibs_serjio_state srj_state;
	const enum nvmeib_recov_src recov_src = fn_param->src;
	enum nvmeibs_serjio_jentry_state jentry_state, next_jentry_state;
	DECLARE_COMPLETION_ONSTACK(comp);
	atomic_t comp_ctr = ATOMIC_INIT(1);
	DECLARE_BITMAP(rng_ents_freed, NVMEIB_EC_MAX_JOURNAL_RANGES) = {0};
	u32 rng_idx;
	unsigned ent_idx;
	u64 free_rng_gen_id;
	struct seg_tree_entry *seg_tree_entry = NULL, *seg_rb_iter;

	NFIN;
	if ((srj_state = serjio_state_get(serjio_pd)) != SERJIO_READY &&
		srj_state != SERJIO_CLN_JRNL) {
		if (SERJIO_HALTING(srj_state)) {
		_NTs(io_free_jrnl_ents_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
		} else if (SERJIO_RUNNING(srj_state)) {
			_NTs(io_free_jrnl_ents_fn_t2, serjio_pd, "SERJIO in running state @SERJIO_STATE - Rescheduling Free Entries", srj_state);
			*resched = true;
			rv = -EAGAIN;
		} else {
			_NTs(io_free_jrnl_ents_fn_t3, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
			rv = -EINVAL;
		}
		goto out;
	}

	if (fn_param->src >= MAX_NVMEIB_RECOV_SRC) {
		_NEs(io_free_jrnl_ents_fn_e1, serjio_pd, "Invalid recov src value (@INT)", fn_param->src);
		rv = -EINVAL;
		goto out;
	}

	_NTs(trace_serjio_io_free_jrnl_ents_fn, serjio_pd,
		 "Free Journal Entries Message for recov @NVMEIB_RECOV_SRC_STR "
		 "of seg: @SEG_UUID_STR with @N_ENTS entries\n",
		 nvmeib_recov_src_str(fn_param->src), fn_param->seg_uuid, fn_param->num_ents);

	if (strncmp(serjio_boot_id, serjio_pd->boot_id, NVMEIB_GID_STR_MAX) != 0) {
		_NTs(trace_1_serjio_io_free_jrnl_ents_fn, serjio_pd, "Free Journal Entries Message has out of date SERJIO Boot ID @SERJIO_BOOT_ID (currently @BOOT_ID)",
			serjio_boot_id, serjio_pd->boot_id);
		rv = -EINVAL;
		goto out;
	}

	if (seg_uuid) {
		/* Find the segment using the id */
		hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_rb_iter, link, hash_uuid_str(seg_uuid)) {
			if (strncmp(seg_rb_iter->seg_uuid_str, seg_uuid, NVMEIB_GID_STR_MAX) == 0) {
				seg_tree_entry = seg_rb_iter;
				break;
			}
		}

		if (!seg_tree_entry) {
			_NEs(error_serjio_io_free_jrnl_ents_fn, serjio_pd,
				 "@NVMEIB_RECOV_SRC_STR: invalid segment @SEG_UUID",
					nvmeib_recov_src_str(recov_src), seg_uuid);
			rv = -ENOENT;
			goto out;
		} else if (recov_src == NVMEIB_RECOV_SRC_JGC) {
			if (seg_tree_entry->jgc_launched_jif) {
				_NTs(trace_serjio_io_free_jrnl_ents_fn_jgc_finished, serjio_pd,
					 "JGC for seg @SEG_UUID_STR finished", seg_uuid);
				seg_tree_entry->jgc_launched_jif = 0;
			} else {
				_NWs(trace_serjio_io_free_jrnl_ents_fn_jgc_unexpected, serjio_pd,
					 "Unexpected JGC response for seg @SEG_UUID_STR", seg_uuid);
			}
		}
	} else if (recov_src != NVMEIB_RECOV_SRC_PROC_FILE) {
		_NWs(warn_serjio_io_free_jrnl_ents_fn_no_seg, serjio_pd,
			 "@NVMEIB_RECOV_SRC_STR: no segment provided", nvmeib_recov_src_str(recov_src));
		rv = -EINVAL;
		goto out;
	}

	for (free_ents = fn_param->free_ents; free_ents < fn_param->free_ents + num_ents; free_ents++) {
		rng_idx = free_ents->rng_idx;
		ent_idx = free_ents->ent_idx;
		free_rng_gen_id = free_ents->rng_gen_id;

		if (rng_idx >= jranges_alloc_tbl->num_ranges) {
			_NEs(error_serjio_io_free_jrnl_ents_fn_inv_rng, serjio_pd,
				 "@NVMEIB_RECOV_SRC_STR: invalid range @JRNL_RNG_IDX",
				nvmeib_recov_src_str(recov_src), rng_idx);
			continue;
		}
		rng = &jranges_alloc_tbl->ranges[rng_idx];

		if ((1U << rng->binje_shift) != free_ents->rng_binje) {
			_NEs(error_serjio_io_free_jrnl_ents_fn_inv_binje, serjio_pd,
				"@NVMEIB_RECOV_SRC_STR: invalid N @BINJE for range @JRNL_RNG_IDX with N @BINJE",
				nvmeib_recov_src_str(recov_src), free_ents->rng_binje, rng->range_idx, (1 << rng->binje_shift));
			continue;
		}

		if (ent_idx >= rng->n_ents) {
			_NEs(error_serjio_io_free_jrnl_ents_fn_inv_entry, serjio_pd,
				"@NVMEIB_RECOV_SRC_STR: invalid entry @JRNL_RNG_ENT_IDX for range @JRNL_RNG_IDX with N @BINJE",
				nvmeib_recov_src_str(recov_src), ent_idx, rng->range_idx, (1 << rng->binje_shift));
			continue;
		}

		spin_lock_irqsave(&rng->lock, flags);
		if (rng->gen_id != free_rng_gen_id ||
			rng->jentry_md[ent_idx].ent_gen_id != free_ents->ent_md.ent_gen_id) {
			_NDs(trace_serjio_io_free_jrnl_ents_fn_gen_mismatch, serjio_pd,
				 "@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX gen_id mismatch @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID != @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID",
				nvmeib_recov_src_str(recov_src), rng_idx, ent_idx, rng->gen_id,
				rng->jentry_md[ent_idx].ent_gen_id, free_rng_gen_id, free_ents->ent_md.ent_gen_id);
			spin_unlock_irqrestore(&rng->lock, flags);
			continue;
		}

		if (rng->status != JRANGE_RESERVED &&
			rng->status != JRANGE_ALLOCATED) {
			_NEs(error_serjio_io_free_jrnl_ents_fn_rng_inv_status, serjio_pd,
				 "@NVMEIB_RECOV_SRC_STR: range @JRNL_RNG_IDX in invalid state @STATUS",
				nvmeib_recov_src_str(recov_src), rng->range_idx, rng->status);
			spin_unlock_irqrestore(&rng->lock, flags);
			continue;
		}

		if (recov_src != NVMEIB_RECOV_SRC_PROC_FILE) {
			/* Check that J2D is in the segment otherwise BUG */
			const union jblock_md *jmdc_entry = get_jmdc_entry(rng, ent_idx);
			const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_entry);
			SERJIO_BUG_ON(j2d < seg_tree_entry->slba || j2d > seg_tree_entry->elba,
						  bug_serjio_io_free_jrnl_ents_fn_invalid_j2d, serjio_pd,
						"@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX "
						"gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID LBA @J2D not in segment "
						"@SEG_UUID [@SLBA_LLONG, @ELBA). BUG!",
						nvmeib_recov_src_str(recov_src), rng->range_idx, ent_idx, free_rng_gen_id,
						free_ents->ent_md.ent_gen_id, j2d, seg_uuid,
						seg_tree_entry->slba, seg_tree_entry->elba);
			if (recov_src == NVMEIB_RECOV_SRC_HTR) {
				/* Source is HTR, entry must be in the locked blockset otherwise BUG */
				SERJIO_BUG_ON(j2d < blkset_slba || j2d >= blkset_slba + LOCKSET_4KS,
							bug_serjio_io_free_jrnl_ents_fn_invalid_j2d_2, serjio_pd,
							"@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX "
							"gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID LBA @J2D not in lockset "
							" [@BLKSET_SLBA, @ELBA). BUG!",
							nvmeib_recov_src_str(recov_src), rng->range_idx, ent_idx, free_rng_gen_id,
							free_ents->ent_md.ent_gen_id, j2d, blkset_slba, blkset_slba + LOCKSET_4KS);
			}
		}

		next_jentry_state = JENTRY_UNKNOWN;
		jentry_state = GET_JENTRY_STATE_FROM_BMP(rng->jentry_state_bmp, ent_idx);

		switch (jentry_state) {
		case JENTRY_TAKEN:
		case JENTRY_WAIT_RET:
			//consider the following use case: client executed transaction and then fails to release locks
			//all entries used for the transaction become free in JAM
			//After pause/continue (on continue the entry will change state from unknown to TAKEN
			//
			//The next transaction will find stale lock and hot recovery will scan and find the journal entries
			//Obviously it will release them (nothing to do) and serjio will complain
			SERJIO_BUG_ON(recov_src != NVMEIB_RECOV_SRC_HTR && recov_src != NVMEIB_RECOV_SRC_PROC_FILE,
						bug_io_free_jrnl_ents_fn_jentry_jam_owned, serjio_pd,
						"Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX "
						"GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID in invalid state: @JENTRY_STATE "
						"for recov src: @NVMEIB_RECOV_SRC_STR",
						rng->range_idx, ent_idx, free_rng_gen_id, free_ents->ent_md.ent_gen_id,
						jentry_state, nvmeib_recov_src_str(recov_src));
			_NTs(trace_serjio_io_free_jrnl_ents_taken_ent, serjio_pd,
				"@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX "
				"gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID in state: @JENTRY_STATE\n",
				nvmeib_recov_src_str(recov_src), rng->range_idx, ent_idx, free_rng_gen_id,
				free_ents->ent_md.ent_gen_id, jentry_state);
			break;
		case JENTRY_FREE:
			SERJIO_BUG_ON(jentry_state == JENTRY_FREE || jentry_state == JENTRY_WAIT_RET,
						bug_serjio_io_free_jrnl_ents_fn_jentry_owned_by_jam, serjio_pd,
						"@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX "
						"gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID is unexpected state: @JENTRY_STATE. BUG!",
						nvmeib_recov_src_str(recov_src), rng->range_idx, ent_idx, free_rng_gen_id,
						free_ents->ent_md.ent_gen_id, jentry_state);
			break;
		case JENTRY_ABND:
		case JENTRY_UNKNOWN:
		case JENTRY_SYNCED:
			_NTs(trace_nvmeibs_serjio_c_6636, serjio_pd, "@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID freeing entry in state: @JENTRY_STATE",
				nvmeib_recov_src_str(recov_src), rng->range_idx, ent_idx, free_rng_gen_id,
				free_ents->ent_md.ent_gen_id, jentry_state);
			next_jentry_state = JENTRY_FREE;
			break;
		case JENTRY_IO_ERR:
			/* Entry had IO Error - Nothing to do */
			break;
		default:
			BUG_ON(1);
		}
		spin_unlock_irqrestore(&rng->lock, flags);

		if (next_jentry_state != JENTRY_FREE) {
			continue;
		}

		set_bit(rng->range_idx, rng_ents_freed);

		/* Clear Journal Entry on Disk */

		_NDs(trace_serjio_io_free_jrnl_ents_fn_free_ent, serjio_pd, "@NVMEIB_RECOV_SRC_STR: range: @JRNL_RNG_IDX entry: @JRNL_RNG_ENT_IDX gen_id @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID - @JENTRY_STATE => FREE",
			nvmeib_recov_src_str(recov_src), rng_idx, ent_idx, rng->gen_id,
			rng->jentry_md[ent_idx].ent_gen_id, jentry_state);
		if ((rv = zero_journal_entry(serjio_pd, rng, ent_idx, &comp_ctr, &comp, NULL, NULL,
			"%s - recov: %s rng %d ent %d gen_id %llx.%x - %s => FREE\n",
			__func__, nvmeib_recov_src_str(recov_src), rng_idx, ent_idx, rng->gen_id,
			rng->jentry_md[ent_idx].ent_gen_id, nvmeib_shared_serjio_jentry_state_to_str(jentry_state)))) {
			_NWs(warn_serjio_io_free_jrnl_ents_fn_zero_failed, serjio_pd, "zero_journal_entry failed (@RV) for range @JRNL_RNG_IDX entry @JRNL_RNG_ENT_IDX",
				rv, rng->range_idx, ent_idx);
		}
		//JJJ: is this where we are missing the SYNCED --> FREE ? assuming rv is ok
		//Also, if zero_journal_entry() failed, should we undo the set-bit above.
	}

	if (atomic_dec_return(&comp_ctr) > 0)
		wait_for_completion(&comp);

	atomic_set(&comp_ctr, 1);
	reinit_completion(&comp);

	/* Check all ranges that were modified */
	for_each_set_bit(rng_idx, rng_ents_freed, NVMEIB_EC_MAX_JOURNAL_RANGES) {
		rng = &jranges_alloc_tbl->ranges[rng_idx];
		sync_jmdc_range_to_all_nics(serjio_pd, rng_idx);
		chk_rng_post_free(rng, &comp, &comp_ctr, JENTRY_STATE_CHNG_REASON_FREE_ENTS);
	}

	if (atomic_dec_return(&comp_ctr) > 0)
		wait_for_completion(&comp);

	/* Check to see if we want to launch a new JGC */
	chk_launch_new_jgc(serjio_pd, false);

out:
	NFOUT;
	return rv;
}

/* Called via no-RDDA Gen Command at the end of the cold recovery process to free journal entries */
int nvmeibs_serjio_free_jrnl_ents(struct nvmeibs_disk_info *di, const char *serjio_boot_id,
								  const char *seg_uuid, enum nvmeib_recov_src recov_src, u64 blkset_slba,
								  int num_ents, const struct nvmeib_free_ents_data *free_ents)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct io_free_jrnl_ents_param fn_param = {
		.serjio_boot_id = serjio_boot_id,
		.seg_uuid = seg_uuid,
		.src = recov_src,
		.blkset_slba = blkset_slba,
		.num_ents = num_ents,
		.free_ents = free_ents,
	};
	int rv;

	NFIN;
	/* Schedule on work-queue */
	rv = run_on_io_wq(serjio_pd, io_free_jrnl_ents_fn, &fn_param, true, true, NVMEIBS_SERJIO_WORK_TYPE_FREE_JRNL_ENTS);
	NFOUT;
	return rv;
}

struct io_abnd_ents_param {
	unsigned long *abnd_ents_bmp;
	u8 *ents_gen_id;
	u32 rng_idx;
};

static DECLARE_IO_WQ_FN(io_abnd_ents_fn)
{
	struct jrange_entry* jrange;
	int rv = 0;
	struct jranges_allocation_table *jranges_alloc_tbl =
		serjio_pd ? &serjio_pd->jranges_alloc_tbl : NULL;
	enum nvmeibs_serjio_state srj_state;
	struct io_abnd_ents_param *fn_param = param;
	u32 rng_idx = fn_param->rng_idx;
	unsigned long *abnd_ents_bmp = fn_param->abnd_ents_bmp;
	u8 *ents_gen_id = fn_param->ents_gen_id;
	unsigned entry = 0;
	int prev_jentry_state;
	unsigned long flags;
	union jblock_md *jmdc_val;
	struct seg_tree_entry *j2d_seg;
	u64 j2d;

	NFIN;
	if ((srj_state = serjio_state_get(serjio_pd)) != SERJIO_READY && srj_state != SERJIO_CLN_JRNL) {
		if (SERJIO_HALTING(srj_state)) {
		_NTs(io_abnd_ents_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", srj_state);
			rv = -ECANCELED;
		} else if (SERJIO_RUNNING(srj_state)) {
			_NTs(io_abnd_ents_fn_t2, serjio_pd, "SERJIO in running state @SERJIO_STATE - Rescheduling Abandon Entries", srj_state);
			*resched = true;
			rv = -EAGAIN;
		} else {
			_NTs(io_abnd_ents_fn_t3, serjio_pd, "SERJIO in invalid state @SERJIO_STATE", srj_state);
			rv = -EINVAL;
		}
		goto out;
	}

	if (rng_idx > jranges_alloc_tbl->num_ranges) {
		_NEs(error_serjio_io_abnd_ents_fn, serjio_pd, "Invalid Journal Range @JRNL_RNG_IDX", rng_idx);
		rv = -EINVAL;
		goto out;
	}
	jrange = &jranges_alloc_tbl->ranges[rng_idx];

	if (jrange->status != JRANGE_ALLOCATED) {
		_NEs(error_1_serjio_io_abnd_ents_fn, serjio_pd, "Journal Range @JRNL_RNG_IDX in invalid state @JRANGE_STATUS",
		   rng_idx, jrange->status);
		rv = -EINVAL;
		goto out;
	}
	_NTs(trace_1_serjio_io_abnd_ents_fn, serjio_pd, "Abandoning entries " NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE " of range @JRNL_RNG_IDX",
		abnd_ents_bmp, jrange->range_idx);
	spin_lock_irqsave(&jrange->lock, flags);
	for_each_set_bit(entry, abnd_ents_bmp, jrange->n_ents) {
		if (jrange->jentry_md[entry].ent_gen_id != ents_gen_id[entry]) {
			/* This isn't a bug because the rediscovery can take place before the unregister took place, so the loser data might reach serjio after the allocation back to JAM already took place.
			   In this case some abandoned entires already changed state (and gen_id) from unknown to synced. */
			enum nvmeibs_serjio_jentry_state state = GET_JENTRY_STATE_FROM_BMP(jrange->jentry_state_bmp, entry);
			_NTs(trace_io_abnd_ents_fn_inv_gen_id, serjio_pd,
				 "Got Abandon for Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX with invalid gen_id @JRNL_ENT_GEN_ID (not @JRNL_ENT_GEN_ID) state: @JENTRY_STATE\n",
				jrange->range_idx, entry, ents_gen_id[entry], jrange->jentry_md[entry].ent_gen_id, state);
			WARN_ON(state != JENTRY_SYNCED);  // when gen_id not matching there must be rellocation of the range to JAM and the state of an abandoned entry must turned sync already.
			spin_unlock_irqrestore(&jrange->lock, flags);
			goto out;
		}
		jmdc_val = get_jmdc_entry(jrange, entry);
		j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmdc_val);
		if (!nvmeib_is_jmd_io_entry(*jmdc_val)) {
			_NTs(trace_serjio_io_abnd_ents_fn_inv_jmdc, serjio_pd,
					"Not abandoning range @JRNL_RNG_IDX entry @JRNL_RNG_ENT_IDX with"
					" jmdc @JMDC_ENT (j2d: @J2D tx_id: @TXID tx_bmp @TXBM_COMPRESSED ver:@X)\n",
					jrange->range_idx, entry, jmdc_val->raw,
					j2d, jmdc_val->tx_id, jmdc_val->tx_bmp, jmdc_val->version);
			continue;
		}
		prev_jentry_state = GET_JENTRY_STATE_FROM_BMP(jrange->jentry_state_bmp, entry);
		if (prev_jentry_state == JENTRY_WAIT_RET) {
			SERJIO_BUG_ON((j2d_seg = get_j2d_cln_rng(serjio_pd, j2d,
													 (1 << CLN_SEG_WAIT_RNG_RETURN), jrange->range_idx, false)),
							error_serjio_io_abnd_ents_fn_inv_j2d, serjio_pd,
							"Got Abandon for Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX "
							" with J2D @J2D in segment @SEG_UUID_STR that is @JRNL_SEG_STATUS ",
							jrange->range_idx, entry, j2d, j2d_seg->seg_uuid_str,
							(j2d_seg->seg_delete ? "DELETED" : (j2d_seg->deprecated ? "DEPRECATED" : "CLEANING")));
		}
		SERJIO_BUG_ON(prev_jentry_state != JENTRY_WAIT_RET &&
					prev_jentry_state != JENTRY_TAKEN,
					error_2_serjio_io_abnd_ents_fn, serjio_pd,
					"Got Abandon for Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX "
					" in invalid state @JENTRY_STATE",
					jrange->range_idx, entry, prev_jentry_state);
		BUG_ON(prev_jentry_state != JENTRY_STATE_CHNG(jrange, entry, JENTRY_ABND, true, JENTRY_STATE_CHNG_REASON_ABND_LOSER));
	}
	spin_unlock_irqrestore(&jrange->lock, flags);
	chk_launch_new_jgc(serjio_pd, false);

out:
	NFOUT;
	return rv;
}

/* Called via the LOSER payload that is appended to TOMA UNREGISTER msg -
 * Used to abandon entries that were used for an IO that failed */
int nvmeibs_serjio_abnd_jrnl_ents(struct nvmeibs_disk_info *di, u32 cl_jrnl_rng,
								  unsigned long *abnd_ents_bmp, u8 *ents_gen_id)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct io_abnd_ents_param fn_param = {
		.rng_idx = cl_jrnl_rng,
		.abnd_ents_bmp = abnd_ents_bmp,
		.ents_gen_id = ents_gen_id,
	};
	int rv;

	NFIN;
	/* Schedule on work-queue */
	rv = run_on_io_wq(serjio_pd, io_abnd_ents_fn, &fn_param, true, true, NVMEIBS_SERJIO_WORK_TYPE_ABND_ENTS);
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_chk_jgc_fn)
{
	int rv = 0;
	enum nvmeibs_serjio_state srj_state;

	NFIN;
	(void)param;
	if ((srj_state = serjio_state_get(serjio_pd)) != SERJIO_READY) {
		_NTs(io_chk_jgc_fn_t1, serjio_pd, "SERJIO in unexpected state @SERJIO_STATE", srj_state);
		rv = -ECANCELED;
		goto out;
	}

	chk_launch_new_jgc(serjio_pd, false);

out:
	NFOUT;
	return rv;
}

const char *nvmeibs_serjio_get_boot_id(struct nvmeibs_disk_info *di)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	return serjio_pd->boot_id;
}

#ifdef SERJIO_DEBUG_GPT
#pragma GCC push_options
#pragma GCC optimize ("O0")
#endif

struct io_gpt_upd_param {
	bool update_done;
	bool primary;
	enum nvmeib_main_gpt_update_flags gpt_update_stage;
	bool init_serjio;
	int user_pid;
	const void __user *gpt_hdr;
	size_t gpt_hdr_sz;
	const void __user *gpt_entries;
	size_t gpt_entries_sz;
};

int nvmeibs_serjio_gpt_update(struct nvmeibs_disk_info *di, bool primary, enum nvmeib_main_gpt_update_flags gpt_update, int user_pid,
							  const void __user *gpt_hdr, size_t gpt_hdr_sz, const void __user *gpt_entries, size_t gpt_entries_sz)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	struct io_gpt_upd_param param = {
		.update_done = false,
		.primary = primary,
		.gpt_update_stage = gpt_update & MAIN_GPT_UPDATE_STAGE_MASK,
		.init_serjio = gpt_update & MAIN_GPT_UPDATE_SERJIO_INIT,
		.user_pid = user_pid,
		.gpt_hdr = gpt_hdr,
		.gpt_hdr_sz = gpt_hdr_sz,
		.gpt_entries = gpt_entries,
		.gpt_entries_sz = gpt_entries_sz,
	};
	int rv;

	if (gpt_hdr_sz < sizeof(struct gpt_header)) {
		_NE(nvmeibs_serjio_gpt_update_e1, "invalid GPT header size @ZU", gpt_hdr_sz);
		rv = -EINVAL;
		goto out;
	}

	if (!serjio_pd) {
		/* SERJIO is not initialized so just say OK */
		rv = 0;
		goto out;
	}

	if (param.gpt_update_stage != MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE &&
		param.gpt_update_stage != MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE) {
		rv = -EINVAL;
		goto out;
	}

	rv = run_on_io_wq(serjio_pd, io_gpt_upd_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD);

out:
	return rv;
}

static int verify_gpt_hdr(struct nvmeibs_serjio_disk_private_data *serjio_pd,
						  struct gpt_header *new_gpt_hdr, struct gpt_entry *new_gpt_ents, size_t *gpt_ents_sz,
						  bool primary, bool post_update)
{
	u64 exp_my_lba = primary ?
		GPT_PRIMARY_HDR_LBA :
		(nvmeibs_disk_info_get_num_blks(serjio_pd->di, true) - 1);
	u64 exp_alt_lba = primary ?
		(nvmeibs_disk_info_get_num_blks(serjio_pd->di, true) - 1) :
		GPT_PRIMARY_HDR_LBA;
	unsigned n_hdr_ents;
	size_t hdr_ents_sz;
	__le32 crc32_tmp, calc_gpt_hdr_crc32;
	int rv;

	if (le64_to_cpu(new_gpt_hdr->my_lba) != exp_my_lba ||
		le64_to_cpu(new_gpt_hdr->alternate_lba) != exp_alt_lba) {
		_NEs(verify_gpt_hdr_e1, serjio_pd, "Invalid GPT Header LBA my: @INT_ULLONG alt: @INT_ULLONG",
		   le64_to_cpu(new_gpt_hdr->my_lba),
		   le64_to_cpu(new_gpt_hdr->alternate_lba));
		rv = -EINVAL;
		goto out;
	}

	if (post_update && le64_to_cpu(new_gpt_hdr->signature) != GPT_HEADER_SIGNATURE) {
		_NEs(verify_gpt_hdr_e2, serjio_pd, "Post-Update GPT Header has invalid signature @_X",
			le64_to_cpu(new_gpt_hdr->signature));
		rv = -EINVAL;
	}

	if (le32_to_cpu(new_gpt_hdr->revision) != GPT_HEADER_REVISION_V1) {
		_NEs(verify_gpt_hdr_e3, serjio_pd, "Invalid GPT Header revision @UINT", le32_to_cpu(new_gpt_hdr->revision));
		rv = -EINVAL;
		goto out;
	}

	if ((n_hdr_ents = gpt_hdr_num_parts(serjio_pd, primary, new_gpt_hdr, new_gpt_ents, *gpt_ents_sz)) <= 0) {
		_NTs(verify_gpt_hdr_inv_n_ents, serjio_pd, "Invalid number of GPT entries @GPT_ENTRIES", n_hdr_ents);
		rv = -EINVAL;
		goto out;
	}

	hdr_ents_sz = n_hdr_ents * le32_to_cpu(new_gpt_hdr->sizeof_partition_entry);

	if (hdr_ents_sz != *gpt_ents_sz) {
		_NTs(verify_gpt_hdr_e4, serjio_pd, "Invalid GPT entries size @ZU", hdr_ents_sz);
		rv = -EINVAL;
		goto out;
	}

	if (post_update) {
		/* Check sig and crc */
		if (le64_to_cpu(new_gpt_hdr->signature) != GPT_HEADER_SIGNATURE) {
			_NEs(verify_gpt_hdr_e5, serjio_pd, "Invalid GPT Header signature @_X", le64_to_cpu(new_gpt_hdr->signature));
			rv = -EINVAL;
			goto out;
		}
		crc32_tmp = new_gpt_hdr->header_crc32;
		new_gpt_hdr->header_crc32 = 0;
		calc_gpt_hdr_crc32 = efi_crc32(new_gpt_hdr, le32_to_cpu(new_gpt_hdr->header_size));
		new_gpt_hdr->header_crc32 = crc32_tmp;
		if (calc_gpt_hdr_crc32 != new_gpt_hdr->header_crc32) {
			_NEs(verify_gpt_hdr_e6, serjio_pd, "Invalid GPT Header CRC @INT32_HEX", le32_to_cpu(new_gpt_hdr->header_crc32));
			rv = -EINVAL;
			goto out;
		}
	}

	rv = n_hdr_ents;
	goto out;

out:
	return rv;
}

struct gpt_upd_sm_data {
	enum nvmeibs_serjio_state serjio_state;
	enum nvmeibs_serjio_state next_srj_state;
	bool update_pending;
	bool update_done;
	enum nvmeib_main_gpt_update_flags gpt_update;
	enum serjio_gpt_upd_state next_gpt_upd_state;
};

const struct gpt_upd_sm_data gpt_upd_sm_data[MAX_SERJIO_GPT_UPDATE] = {
	[SERJIO_GPT_UPDATE_IDLE] = {
		.serjio_state = SERJIO_READY,
		.next_srj_state = SERJIO_GPT_UPDATE,
		.update_pending = true,
		.update_done = false,
		.gpt_update = MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE,
		.next_gpt_upd_state = SERJIO_CHECK_GPT_PRE_WRITE,
	},
	[SERJIO_CHECK_GPT_PRE_WRITE] = {
		.serjio_state = SERJIO_GPT_UPDATE,
		.next_srj_state = SERJIO_GPT_UPDATE,
		.update_pending = false,
		.update_done = true,
		.gpt_update = MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE,
		.next_gpt_upd_state = SERJIO_GPT_PRE_WRITE_DONE,
	},
	[SERJIO_GPT_PRE_WRITE_DONE] = {
		.serjio_state = SERJIO_GPT_UPDATE,
		.next_srj_state = SERJIO_GPT_UPDATE,
		.update_pending = false,
		.update_done = false,
		.gpt_update = MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE,
		.next_gpt_upd_state = SERJIO_CHECK_GPT_POST_WRITE,
	},
	[SERJIO_CHECK_GPT_POST_WRITE] = {
		.serjio_state = SERJIO_GPT_UPDATE,
		.next_srj_state = SERJIO_GPT_UPDATE,
		.update_pending = false,
		.update_done = true,
		.gpt_update = MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE,
		.next_gpt_upd_state = SERJIO_GPT_POST_WRITE_DONE,
	},
	[SERJIO_GPT_POST_WRITE_DONE] = {
		.serjio_state = SERJIO_GPT_UPDATE,
		.next_srj_state = SERJIO_READY,
		.update_pending = false,
		.update_done = false,
		.gpt_update = NO_MAIN_GPT_UPDATE,
		.next_gpt_upd_state = SERJIO_GPT_UPDATE_IDLE,
	},
	[SERJIO_GPT_UPDATE_CANCELLED] = {
		.serjio_state = SERJIO_GPT_UPDATE,
		.next_srj_state = SERJIO_READY,
		.update_pending = false,
		.update_done = false,
		.gpt_update = NO_MAIN_GPT_UPDATE,
		.next_gpt_upd_state = SERJIO_GPT_UPDATE_IDLE,
	},
};

static DECLARE_IO_WQ_FN(io_gpt_upd_fn)
{
	struct io_gpt_upd_param *fn_param = param;
	struct gpt_header *new_gpt_hdr = NULL;
	struct gpt_entry *new_gpt_ents = NULL, *new_gpt_ent;
	unsigned n_new_gpt_ents;
	int rv, i, zero_ent_start;
	enum nvmeibs_serjio_state srj_state = serjio_state_get(serjio_pd);
	const struct gpt_upd_sm_data *sm_data = &gpt_upd_sm_data[serjio_pd->gpt_upd_state];
	struct nvmeibs_serjio_disk_ranges new_disk_ranges = {
		.journal = {
			.part_idx = -1,
		},
		.db = {
			.part_idx = -1,
		}
	};
	NFIN;

	_NTs(trace_serjio_io_gpt_upd_fn, serjio_pd, "GPT Update @GPT_UPDATE_STR (Init SERJIO @BOOL_YN) -"
		" Update State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE) -"
		" Expecting Update @GPT_UPDATE_STR - Next State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE)",
		nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary),
		fn_param->init_serjio,
		get_serjio_gpt_upd_state_str(serjio_pd->gpt_upd_state),
		sm_data->serjio_state,
		nvmeib_gpt_update_str(sm_data->gpt_update, fn_param->primary),
		get_serjio_gpt_upd_state_str(sm_data->next_gpt_upd_state),
		sm_data->next_srj_state);

	if (!(new_gpt_hdr = kzalloc(fn_param->gpt_hdr_sz, GFP_KERNEL)) ||
		!(new_gpt_ents = vzalloc(fn_param->gpt_entries_sz))) {
		_NEs(error_serjio_io_gpt_upd_fn_oom, serjio_pd, "OOM Error");
		rv = -ENOMEM;
		goto free_mem;
	}

	/* Copy GPT Header and Entries from user-space */
	if ((rv = nvmeib_public_copy_user_pages(new_gpt_ents,
			fn_param->user_pid, (void *)fn_param->gpt_entries,
			fn_param->gpt_entries_sz, 0)) < 0) {
		_NEs(error_serjio_io_gpt_upd_fn_copy_user_pgs, serjio_pd, "Error copying GPT entries");
		rv = -EFAULT;
		goto free_mem;
	}

	if ((rv = nvmeib_public_copy_user_pages(new_gpt_hdr,
			fn_param->user_pid, (void *)fn_param->gpt_hdr,
			fn_param->gpt_hdr_sz, 0)) < 0) {
		_NEs(error_1_serjio_io_gpt_upd_fn_copy_user_pgs, serjio_pd, "Error copying GPT header");
		rv = -EFAULT;
		goto free_mem;
	}

	/* Verify GPT Header */
	if ((rv = verify_gpt_hdr(serjio_pd, new_gpt_hdr, new_gpt_ents,
		&fn_param->gpt_entries_sz, fn_param->primary,
		fn_param->gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE)) < 0)
		goto free_mem;

	for (i = 0, new_gpt_ent = new_gpt_ents, zero_ent_start = -1; (void *)new_gpt_ent < (void *)new_gpt_ents + fn_param->gpt_entries_sz; new_gpt_ent++, i++) {
		char part_name[80] = "";
		int c;

		/* Don't full log the zero entries */
		if (memcmp(&gpt_zero_entry, new_gpt_ent, sizeof(*new_gpt_ent)) == 0) {
			if (zero_ent_start == -1)
				zero_ent_start = i;
			continue;
		} else if (zero_ent_start >= 0) {
			_NTs(trace_serjio_io_gpt_upd_fn_print_ents_zero, serjio_pd,
				 "GPT Entries [@GPT_IDX - @GPT_IDX): 0", zero_ent_start, i);
			zero_ent_start = -1;
		}

		for (c = 0; c < (int)ARRAY_SIZE(new_gpt_ent->partition_name); c++)
			part_name[c] = new_gpt_ent->partition_name[c] & 0xff;

		_NTs(trace_serjio_io_gpt_upd_fn_new_ents, serjio_pd,
			 "GPT Entry [@GPT_IDX]: Type GUID: @PARTITION_TYPE_GUID"
			" Unique GUID: @UNIQUE_PARTITION_GUID SLBA: @SLBA_LLONG ELBA: @ELBA"
			" Attributes: @GPT_PARTITION_ATTRIBUTES Name: @GPT_PARTITION_NAME",
			i, &new_gpt_ent->partition_type_guid, &new_gpt_ent->unique_partition_guid,
			le64_to_cpu(new_gpt_ent->starting_lba), le64_to_cpu(new_gpt_ent->ending_lba),
			le64_to_cpu(new_gpt_ent->attributes), part_name);

		if (is_gpt_ent_serjio_db(new_gpt_ent)) {
			u64 new_slba = le64_to_cpu(new_gpt_ent->starting_lba);
			u64 new_elba = le64_to_cpu(new_gpt_ent->ending_lba);

			new_disk_ranges.db.lba = new_slba;
			new_disk_ranges.db.len_nlbas = new_elba - new_slba + 1;
			new_disk_ranges.db.part_idx = i;
			new_disk_ranges.db.part_uuid = new_gpt_ent->unique_partition_guid;

			_NTs(trace_3_serjio_io_gpt_upd_fn, serjio_pd,
				"Found SERJIO DB in updated GPT - index: @GPT_IDX lba: [@SLBA_LLONG, @ELBA]",
				i, new_slba, new_elba);
		} else if (is_gpt_ent_jrnl(new_gpt_ent)) {
			u64 new_slba = le64_to_cpu(new_gpt_ent->starting_lba);
			u64 new_elba = le64_to_cpu(new_gpt_ent->ending_lba);
			
			new_disk_ranges.journal.lba = new_slba;
			new_disk_ranges.journal.len_nlbas = new_elba - new_slba + 1;
			new_disk_ranges.journal.part_idx = i;
			new_disk_ranges.journal.part_uuid = new_gpt_ent->unique_partition_guid;

			_NTs(trace_4_serjio_io_gpt_upd_fn, serjio_pd,
			     "Found Journal in updated GPT - index: @GPT_IDX lba: [@SLBA_LLONG, @ELBA]",
				i, new_slba, new_elba);
		}
	}
	if (zero_ent_start >= 0) {
		_NTs(trace_serjio_io_gpt_upd_fn_print_ents_zero_2, serjio_pd,
			 "GPT Entries [@GPT_IDX - @GPT_IDX): 0", zero_ent_start, i);
	}

	BUG_ON(fn_param->update_done);

	if (srj_state != sm_data->serjio_state) {
		if (SERJIO_HALTING(srj_state)) {
			_NTs(trace_serjio_io_gpt_upd_fn_shutdown_state, serjio_pd,
				 "SERJIO in shut-down state @SERJIO_STATE - Okaying GPT update @GPT_UPDATE_STR",
				srj_state, nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary));
			rv = 0;
		} else if (serjio_pd->gpt_upd_state == SERJIO_GPT_UPDATE_IDLE) {
			if (SERJIO_RUNNING(srj_state)) {
				_NTs(trace_serjio_io_gpt_upd_fn_run_state, serjio_pd,
					"SERJIO in running state @SERJIO_STATE - Rescheduling GPT Update @GPT_UPDATE_STR",
					srj_state, nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary));
				*resched = true;
				rv = -EAGAIN;
			} else if (SERJIO_BOOTING(srj_state)) {
				_NTs(trace_serjio_io_gpt_upd_fn_boot_state, serjio_pd,
					 "SERJIO in booting state @SERJIO_STATE - Rescheduling GPT Update @GPT_UPDATE_STR",
					srj_state, nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary));
				*resched = true;
				rv = -EAGAIN;
			} else {
				_NTs(trace_serjio_io_gpt_upd_fn_inv_state, serjio_pd,
					 "SERJIO in invalid state @SERJIO_STATE", srj_state);
				rv = -ECANCELED;
				BUG_ON(1);
			}
			goto free_mem;
		} else {
			_NTs(trace_serjio_io_gpt_upd_fn_inv_state2, serjio_pd,
				 "SERJIO in invalid state @SERJIO_STATE for GPT State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE)",
				srj_state, get_serjio_gpt_upd_state_str(serjio_pd->gpt_upd_state), srj_state);
			rv = -ECANCELED;
			BUG_ON(1);
		}
		goto free_mem;
	}
	
	if (new_disk_ranges.journal.part_idx < 0) {
		_NEs(error_serjio_io_gpt_upd_fn_jrnl_missing, serjio_pd,
		     "Journal missing in new GPT");
		rv = -ENOENT;
		goto free_mem;
	}
	if (new_disk_ranges.db.part_idx < 0) {
		_NEs(error_serjio_io_gpt_upd_fn_srj_db_missing, serjio_pd,
		     "SERJIO DB missing in new GPT");
		rv = -ENOENT;
		goto free_mem;
	}

	if (fn_param->init_serjio) {
		/* Got init serjio flag while SERJIO is running - move to GPT Error state to prepare */
		_NTs(io_gpt_upd_fn_init_srj_while_ready, serjio_pd,
		     "Moving to GPT Error state to prepare for SERJIO Init");
		serjio_go_to_err_state(serjio_pd, SERJIO_ERR_GPT);
		rv = 0;
		goto free_mem;
	}

	BUG_ON(fn_param->update_done != sm_data->update_done);
	BUG_ON(fn_param->gpt_update_stage != sm_data->gpt_update);

	n_new_gpt_ents = gpt_hdr_num_parts(serjio_pd, fn_param->primary, new_gpt_hdr,
									   new_gpt_ents, fn_param->gpt_entries_sz);

	if (!sm_data->update_pending) {
		if (fn_param->primary != serjio_pd->pend_gpt_primary) {
			_NEs(error_serjio_io_gpt_upd_fn_gpt_primary_chng, serjio_pd,
				 "Primary flag changed between GPT updates");
			rv = -EINVAL;
			goto free_mem;
		}

		/* Compare Entries with already stored pending copy */
		if (n_new_gpt_ents != serjio_pd->n_pend_gpt_ents) {
			_NEs(error_serjio_io_gpt_upd_fn_num_ents_chng, serjio_pd,
				 "Number of partition entries changed between GPT updates");
			rv = -EINVAL;
			goto free_mem;
		}

		/* Compare Entries with already stored pending copy */
		if (memcmp(new_gpt_ents,
		serjio_pd->pend_gpt_ents, serjio_pd->pend_gpt_ents_sz) != 0) {
			_NEs(error_serjio_io_gpt_upd_fn_ents_chng, serjio_pd,
				 "Partition entries changed between GPT updates");
			rv = -EINVAL;
			goto free_mem;
		}

		_NTs(trace_2_serjio_io_gpt_upd_fn, serjio_pd,
			 "GPT Update matches pending. Ok");
		BUG_ON(serjio_state_cmp_exch(serjio_pd, sm_data->serjio_state,
								 sm_data->next_srj_state) != sm_data->serjio_state);
		serjio_pd->gpt_upd_state = sm_data->next_gpt_upd_state;
		rv = 0;
		goto free_mem;
	}

	/* Check and then update pending GPT */

	/* Check for Journal and SERJIO DB partitions and ensure they haven't been moved.
	* If they didn't exist, then we would be in state SERJIO_ERROR and not be at this point */
	if (new_disk_ranges.db.lba != serjio_pd->disk_ranges.db.lba ||
		new_disk_ranges.db.len_nlbas != serjio_pd->disk_ranges.db.len_nlbas)
	{
		_NEs(error_serjio_io_gpt_upd_fn_serjio_db_moved, serjio_pd,
			"SERJIO DB in updated GPT unexpectedly moved to [@SLBA_LLONG, @ELBA]", 
			new_disk_ranges.db.lba, new_disk_ranges.db.lba + new_disk_ranges.db.len_nlbas - 1);
		rv = -EINVAL;
		goto free_mem;
	}
	if (new_disk_ranges.journal.lba != serjio_pd->disk_ranges.journal.lba ||
		new_disk_ranges.journal.len_nlbas != serjio_pd->disk_ranges.journal.len_nlbas) 
	{
		_NEs(error_serjio_io_gpt_upd_fn_jrnl_moved, serjio_pd,
		     "Journal in new updated unexpectedly moved to [@SLBA_LLONG, @ELBA]",
			new_disk_ranges.journal.lba, new_disk_ranges.journal.lba + new_disk_ranges.journal.len_nlbas - 1);
		rv = -EINVAL;
		goto free_mem;
	}
	if ((rv = update_jrnl_segs_from_gpt(serjio_pd, new_gpt_ents,
			n_new_gpt_ents, GPT_VERIFY, NULL)) < 0)
		goto free_mem;

	serjio_pd->pend_gpt_primary = fn_param->primary;
	serjio_pd->pend_gpt_hdr = new_gpt_hdr;
	serjio_pd->pend_gpt_ents = new_gpt_ents;
	serjio_pd->n_pend_gpt_ents = n_new_gpt_ents;
	serjio_pd->pend_gpt_ents_sz = fn_param->gpt_entries_sz;

	BUG_ON(serjio_state_cmp_exch(serjio_pd, sm_data->serjio_state,
								 sm_data->next_srj_state) != sm_data->serjio_state);
	serjio_pd->gpt_upd_state = sm_data->next_gpt_upd_state;

	rv = 0;
	goto out;

free_mem:
	kfree(new_gpt_hdr);
	vfree(new_gpt_ents);

out:
	if (rv < 0 && serjio_pd->gpt_upd_state != SERJIO_GPT_UPDATE_IDLE) {
		/* Failed update and not in idle => Go to error state */
		serjio_go_to_err_state(serjio_pd, SERJIO_ERR_GPT);
	}
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_gpt_upd_done_fn)
{
	struct io_gpt_upd_param *fn_param = param;
	int rv;
	enum nvmeibs_serjio_state srj_state;
	const struct gpt_upd_sm_data *sm_data = &gpt_upd_sm_data[serjio_pd->gpt_upd_state];
	NFIN;

	BUG_ON(!fn_param->update_done);

	srj_state = serjio_state_get(serjio_pd);
	if (srj_state != SERJIO_GPT_UPDATE) {
		if (SERJIO_HALTING(srj_state)) {
			/* Check to see if we are in GPT Error State and the final GPT update has finished */
			if (srj_state == SERJIO_ERR_GPT && fn_param->primary &&
				fn_param->gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE) {
				/* New GPT has been written, move to INIT state and re-read GPT */
				serjio_pd->gpt_upd_state = SERJIO_GPT_UPDATE_IDLE;
				if (fn_param->init_serjio) {
					/* [NVMESH-4781]: Delay the GPT Write Disk Completion until SERJIO Inits the Journal */
					_NTs(trace_serjio_io_gpt_upd_done_fn_error_to_init_jrnl, serjio_pd,
					     "GPT Written with Init SERJIO Flag. Reading new GPT and then Init DB/Journal");
					serjio_state_exch(serjio_pd, SERJIO_GPT_INIT);
					if (nvmeibs_serjio_fail_next_gpt_init) {
						nvmeibs_serjio_fail_next_gpt_init = false;
						rv = -ECANCELED;
					} else {
						rv = run_on_io_wq(serjio_pd, io_rd_gpt_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_RD_GPT);
					}
					if (!rv) {
						_NTs(trace_serjio_io_gpt_upd_done_fn_init_jrnl_ok, serjio_pd,
						     "SERJIO Init Complete");
					} else {
						_NTs(trace_serjio_io_gpt_upd_done_fn_init_jrnl_fail, serjio_pd,
						     "SERJIO Init Failed (@RV)", rv);
						serjio_state_cmp_exch(serjio_pd, SERJIO_GPT_INIT, SERJIO_ERR_GPT);
					}
					goto out;
				} else {
					_NTs(trace_serjio_io_gpt_upd_done_fn_error_to_init, serjio_pd,
					     "Finished updating GPT. Moving to state @SERJIO_STATE", SERJIO_INIT);
					serjio_state_exch(serjio_pd, SERJIO_INIT);
					if ((rv = run_on_io_wq(serjio_pd, io_rd_gpt_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_GPT)) == -EINPROGRESS)
						rv = 0;
					if (rv < 0)
						serjio_state_cmp_exch(serjio_pd, SERJIO_INIT, SERJIO_ERR_GPT);
				}
			} else {
				_NTs(trace_serjio_io_gpt_upd_done_fn_shutdown_state, serjio_pd,
					 "SERJIO in shut-down state @SERJIO_STATE - Okaying GPT update @GPT_UPDATE_STR done", srj_state,
					nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary));
				rv = 0;
			}
			goto out;
		} else {
			_NTs(trace_serjio_io_gpt_upd_done_fn_inv_state, serjio_pd,
				 "SERJIO in invalid state @SERJIO_STATE for GPT State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE)",
				srj_state, get_serjio_gpt_upd_state_str(serjio_pd->gpt_upd_state), serjio_pd->gpt_upd_state);
			BUG_ON(1);
			rv = -EINVAL;
			goto out;
		}
	}

	_NTs(trace_serjio_io_gpt_upd_done_fn, serjio_pd, "GPT Update @GPT_UPDATE_STR (Init SERJIO @BOOL_YN) Done -"
		" Update State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE) -"
		" Expecting Update @GPT_UPDATE_STR @SERJIO_GPT_UPDATE_SM_UPD_DONE_STR -"
		" Next State @SERJIO_GPT_UPDATE_STR (@SERJIO_STATE) \n",
		nvmeib_gpt_update_str(fn_param->gpt_update_stage, fn_param->primary), fn_param->init_serjio,
		get_serjio_gpt_upd_state_str(serjio_pd->gpt_upd_state),
		sm_data->serjio_state,
		nvmeib_gpt_update_str(sm_data->gpt_update, fn_param->primary),
		sm_data->update_done ? "Done" : "Check",
		get_serjio_gpt_upd_state_str(sm_data->next_gpt_upd_state),
		sm_data->next_srj_state);

	BUG_ON(!sm_data->update_done);
	BUG_ON(fn_param->gpt_update_stage != sm_data->gpt_update);
	BUG_ON(fn_param->primary != serjio_pd->pend_gpt_primary);

	SERJIO_BUG_ON(fn_param->init_serjio, bug_gpt_upd_done_init_serjio_inv_state,
		      serjio_pd, "Init SERJIO flag not valid when SERJIO running");

	serjio_pd->gpt_upd_state = sm_data->next_gpt_upd_state;
	if (serjio_pd->gpt_upd_state == SERJIO_GPT_POST_WRITE_DONE) {
		if (serjio_pd->pend_gpt_primary) {
			/* Updated primary GPT, re-read it and update SERJIO */
			if ((rv = run_on_io_wq(serjio_pd, io_rd_gpt_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_GPT)) == -EINPROGRESS)
				rv = 0;
			if (rv < 0)
				goto error;
		} else {
			/* Updated alternate GPT. Move back to ready state */
			kfree(serjio_pd->pend_gpt_hdr);
			serjio_pd->pend_gpt_hdr = NULL;
			vfree(serjio_pd->pend_gpt_ents);
			serjio_pd->pend_gpt_ents = NULL;
			serjio_pd->n_pend_gpt_ents = 0;
			BUG_ON(serjio_state_cmp_exch(
				serjio_pd, SERJIO_GPT_UPDATE, SERJIO_READY) != SERJIO_GPT_UPDATE);
			serjio_pd->gpt_upd_state = SERJIO_GPT_UPDATE_IDLE;
		}
	}
	rv = 0;
	goto out;

error:
	if (rv < 0) {
		/* Attempting to write invalid GPT => Go to error state */
		serjio_go_to_err_state(serjio_pd, SERJIO_ERR_GPT);
	}

out:
	NFOUT;
	return rv;
}

static DECLARE_IO_WQ_FN(io_gpt_update_cancel_fn);

int nvmeibs_serjio_gpt_update_done(struct nvmeibs_disk_info *di, bool primary,
				enum nvmeib_main_gpt_update_flags gpt_update, int status)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	DECLARE_COMPLETION_ONSTACK(init_done);
	struct io_gpt_upd_param param = {
		.update_done = true,
		.primary = primary,
		.gpt_update_stage = gpt_update & MAIN_GPT_UPDATE_STAGE_MASK,
		.init_serjio = gpt_update & MAIN_GPT_UPDATE_SERJIO_INIT,
	};
	int rv;

	if (!serjio_pd) {
		/* SERJIO is not initialized so just say OK */
		rv = 0;
		goto out;
	}
	
	if (status >= 0)
		rv = run_on_io_wq(serjio_pd, io_gpt_upd_done_fn, &param, true, true, NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_DONE);
	else
		rv = run_on_io_wq(serjio_pd, io_gpt_update_cancel_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_CANCEL);

out:
	return rv;
}

static DECLARE_IO_WQ_FN(io_gpt_update_cancel_fn)
{
	enum nvmeibs_serjio_state srj_state;
	int rv;

	(void)param;
	srj_state = serjio_state_get(serjio_pd);

	if (srj_state == SERJIO_GPT_UPDATE) {
		_NTs(trace_io_gpt_cancel_fn, serjio_pd, "Cancelled GPT Update. Moving to INIT State and re-reading GPT");

		/* Run Read-GPT work.
		 * If the Primary GPT has been corrupted by the cancelled write,
		 * then the read-gpt will fail and SERJIO will move to the error state
		 * otherwise, SERJIO will return to the ready state
		 */

		serjio_pd->gpt_upd_state = SERJIO_GPT_UPDATE_CANCELLED;

		if ((rv = run_on_io_wq(serjio_pd, io_rd_gpt_fn, NULL, false, true, NVMEIBS_SERJIO_WORK_TYPE_RD_GPT)) == -EINPROGRESS)
			rv = 0;
	}

	return 0;
}

int nvmeibs_serjio_gpt_update_cancel(struct nvmeibs_disk_info *di)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd =
		nvmeibs_disk_info_get_serjio_private_data(di);
	int rv;

	if (!serjio_pd) {
		_NT(trace_serjio_gpt_update_cancel_null_serjio, "SERJIO: Disk has NULL serjio_pd");
		rv = -ENOENT;
		goto out;
	}

	rv = run_on_io_wq(serjio_pd, io_gpt_update_cancel_fn, NULL, true, true, NVMEIBS_SERJIO_WORK_TYPE_GPT_UPD_CANCEL);

out:
	return rv;
}

#ifdef SERJIO_DEBUG_GPT
#pragma GCC pop_options
#endif

struct erase_jam_ent_param {
	u64 rng_gen_id;
	u32 rng_idx;
	int num_ents;
	const struct nvmeib_gen_cmd_jam_ent_erase *erase_ents;
	enum nvmeib_gen_cmd_jam_ent_erase_reason erase_reason;
};

struct erase_cb_param {
	spinlock_t lock;
	DECLARE_BITMAP(fail_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	int status;
};

static void erase_journal_entry_cb(void *arg, int status, u32 result)
{
	struct nvme_op_rsrc *op_rsrc = arg;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = op_rsrc->serjio_pd;
	int range_idx = op_rsrc->range_idx;
	int entry_idx = op_rsrc->entry;
	struct jrange_entry *jrange_entry = &serjio_pd->jranges_alloc_tbl.ranges[range_idx];
	union jblock_md *jmdc_entry = get_jmdc_entry(jrange_entry, entry_idx);
	unsigned long flags;
	struct erase_cb_param *cb_param = op_rsrc->param;

	(void)result;
	nvme_op_rsrc_chng_state(op_rsrc, NVME_OP_POSTED, NVME_OP_CB);

	/* Check state is owned by JAM */
	spin_lock_irqsave(&jrange_entry->lock, flags);
	BUG_ON(!(JENTRY_OWNED_BY_JAM(GET_JENTRY_STATE_FROM_BMP(jrange_entry->jentry_state_bmp, entry_idx))));
	spin_unlock_irqrestore(&jrange_entry->lock, flags);

	if (status) {
		spin_lock_irqsave(&cb_param->lock, flags);
		set_bit(entry_idx, cb_param->fail_bmp);
		cb_param->status = cb_param->status ?: status;
		spin_unlock_irqrestore(&cb_param->lock, flags);
		nvmeib_shared_set_jentry_md_invalid_special(jmdc_entry, 1 << jrange_entry->binje_shift);
		_NTs(error_nvmeibs_serjio_c_7380, serjio_pd,
			"Error (@STATUS) erasing journal entry (@JRNL_RNG_IDX,@JRNL_RNG_ENT_IDX)",
			status, range_idx, entry_idx);
	} else {
		nvmeib_shared_set_jentry_md_unused(jmdc_entry, 1 << jrange_entry->binje_shift);
		_NDs(trace_nvmeibs_serjio_c_7385, serjio_pd,
			 "Erased Journal Range @JRNL_RNG_IDX Entry @JRNL_RNG_ENT_IDX",
			range_idx, entry_idx);
	}
	op_rsrc->status = status;
	if (op_rsrc->comp) {
		if (!op_rsrc->comp_ctr || atomic_dec_return(op_rsrc->comp_ctr) == 0)
				complete(op_rsrc->comp);
	}
	return_nvme_op_rsrc_sync(op_rsrc->serjio_pd, op_rsrc, NVME_OP_CB);
}

int nvmeibs_serjio_erase_jam_ent(void *jrange_handle, const binje_t rng_binje, u64 rng_gen_id, int num_ents,
								 const struct nvmeib_gen_cmd_jam_ent_erase *erase_ents,
								 enum nvmeib_gen_cmd_jam_ent_erase_reason erase_reason)
{
	struct jrange_entry* jrange = jrange_handle;
	unsigned long flags;
	atomic_t comp_ctr = ATOMIC_INIT(1);
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv, i;
	struct erase_cb_param cb_param;
	enum nvmeibs_serjio_jentry_state jentry_state;
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	const struct nvmeib_gen_cmd_jam_ent_erase *erase_ent;

	NFIN;
	memset(&cb_param, 0, sizeof(cb_param));
	if (!jrange) {
		_NE(error_serjio_erase_jam_ent_null_handle, "NULL Journal Range Handle");
		rv = NVMEIBS_IO_RSP_ERR_GENERIC;
		goto out;
	}
	BUG_ON(jrange->status != JRANGE_ALLOCATED);
	serjio_pd = jrange->serjio_pd;

	if (jrange->gen_id != rng_gen_id) {
		_NWs(warn_nvmeibs_serjio_c_7400, serjio_pd, "Got Entry Erase for Range: @JRNL_RNG_IDX"
		" with invalid GenID: @JRNL_RNG_GEN_ID (not @JRNL_RNG_GEN_ID)",
		jrange->range_idx, rng_gen_id, jrange->gen_id);
		rv = NVMEIBS_IO_RSP_ERR_INV_JRNG_GENID;
		goto out;
	}

	if ((1U << jrange->binje_shift) != rng_binje) {
		_NWs(warn_serjio_erase_jam_ent_inv_binje, serjio_pd, "Got Entry Erase for Range: @JRNL_RNG_IDX"
			" with invalid N: @BINJE (not @BINJE)",
			jrange->range_idx, rng_binje, 1 << jrange->binje_shift);
		rv = NVMEIBS_IO_RSP_ERR_INV_JRNG_GENID;
		goto out;
	}

	spin_lock_init(&cb_param.lock);
	for (i = 0, erase_ent = erase_ents; i < num_ents; i++, erase_ent++) {
		u64 exp_swlba = JOURNAL_RANGE_ENT_TO_NVMEIBC_SECT(serjio_pd, jrange->range_idx, erase_ent->ent_idx);
		if (erase_ent->ent_swlba != exp_swlba) {
			_NWs(warn_nvmeibs_serjio_c_7462, serjio_pd, "Got Entry Erase for Range: @JRNL_RNG_IDX"
			" Entry: @JRNL_RNG_ENT_IDX with invalid SW LBA: @SW_LBA (not @SW_LBA)",
				 jrange->range_idx, erase_ent->ent_idx, erase_ent->ent_swlba, exp_swlba);
			rv = NVMEIBS_IO_RSP_ERR_INV_JENT_SWLBA;
			goto out;
		}
		if (erase_ent->ent_md.ent_gen_id != jrange->jentry_md[erase_ent->ent_idx].ent_gen_id) {
			_NWs(warn_nvmeibs_serjio_c_7469, serjio_pd, "Got Entry Erase for Range: @JRNL_RNG_IDX"
			" Entry: @JRNL_RNG_ENT_IDX with invalid GenID: @JRNL_ENT_GEN_ID (not @JRNL_ENT_GEN_ID)",
				 jrange->range_idx, erase_ent->ent_idx, erase_ent->ent_md.ent_gen_id, jrange->jentry_md[erase_ent->ent_idx].ent_gen_id);
			rv = NVMEIBS_IO_RSP_ERR_INV_JENT_GENID;
			goto out;
		}
		spin_lock_irqsave(&jrange->lock, flags);
		jentry_state = GET_JENTRY_STATE_FROM_BMP(jrange->jentry_state_bmp, erase_ent->ent_idx);
		if (!(JENTRY_OWNED_BY_JAM(jentry_state))) {
			spin_unlock_irqrestore(&jrange->lock, flags);
			_NWs(warn_nvmeibs_serjio_c_7479, serjio_pd, "Got Entry Erase for Range: @JRNL_RNG_IDX"
			" Entry: @JRNL_RNG_ENT_IDX in invalid state: @JENTRY_STATE",
			jrange->range_idx, erase_ent->ent_idx, jentry_state);
			BUG_ON(1);
			rv = NVMEIBS_IO_RSP_ERR_INV_JENT_STATE;
			goto out;
		}
		spin_unlock_irqrestore(&jrange->lock, flags);
		if ((rv = zero_journal_entry(serjio_pd, jrange, erase_ent->ent_idx, &comp_ctr,
			&comp, erase_journal_entry_cb, &cb_param,
			"%s - rng: %u entry: %d", __func__, jrange->range_idx, i)) < 0) {
			_NWs(warn_nvmeibs_serjio_c_7490, serjio_pd, "Failed to erase Range: @JRNL_RNG_IDX"
			" Entry: @JRNL_RNG_ENT_IDX", jrange->range_idx, erase_ent->ent_idx);
			rv = NVMEIBS_IO_RSP_ERR_SUBMIT;
			break;
		}
		_NTs(trace_nvmeibs_serjio_c_7494, serjio_pd, "Erased Range: @JRNL_RNG_IDX"
			" Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID due to @JAM_ERASE_REASON",
			jrange->range_idx, erase_ent->ent_idx, jrange->gen_id,
			jrange->jentry_md[erase_ent->ent_idx].ent_gen_id,
			get_jam_ent_erase_reason(erase_reason));
	}

	if (atomic_dec_return(&comp_ctr) > 0)
		wait_for_completion(&comp);

	if (cb_param.status) {
		_NWs(warn_nvmeibs_serjio_c_7502, serjio_pd, "Erase failed for Entries: "
		NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE "for Range: @JRNL_RNG_IDX with Status (@STATUS)",
			 cb_param.fail_bmp, jrange->range_idx, cb_param.status);
		rv = NVMEIBS_IO_RSP_ERR_SERJIO;
		goto out;
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

int nvmeibs_serjio_get_seg_range(struct nvmeibs_disk_info *di, const char *seg_uuid_str, u64 *start_lba, u64 *end_lba)
{
	struct nvmeibs_serjio_disk_private_data *serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di);
	struct seg_tree_entry *seg_ent_iter;
	struct gpt_entry *gpt_entry;
	char ent_uuid_str[NVMEIB_GID_STR_MAX];

	int rv = -ENOENT;
	NFIN;
	if (!serjio_pd) {
		_NE(err_serjio_get_seg_range_null_pd, "serjio_pd not found for disk @DISK_NAME",
			di->disk_id);
		rv = -ENODATA;
		goto out;
	}
	if (IS_SERJIO_DYING(serjio_pd)) {
		_NEs(err_serjio_get_seg_range_not_ready, serjio_pd, "not ready");
		rv = -ENODATA;
		goto out;
	}
	down_read(&serjio_pd->gpt_rwsem);
	/* First look to see if it is a journalled segment */
	hash_for_each_possible(serjio_pd->jrnl_seg_tbl, seg_ent_iter, link,
									hash_uuid_str(seg_uuid_str)) {
		if (strncmp(seg_uuid_str, seg_ent_iter->seg_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
			*start_lba = seg_ent_iter->slba;
			*end_lba = seg_ent_iter->elba;
			rv = 0;
			goto unlock;
		}
	}
	/* It isn't, need to search the whole GPT */
	for (gpt_entry = serjio_pd->gpt_entry;
		(void *)gpt_entry < (void *)serjio_pd->gpt_entry + serjio_pd->gpt_ents_sz; gpt_entry++) {
		nvmeibs_serjio_uuid_le_to_str(ent_uuid_str, gpt_entry->unique_partition_guid.b);
			if (strncmp(seg_uuid_str, ent_uuid_str, NVMEIB_GID_STR_MAX) == 0) {
			*start_lba = le64_to_cpu(gpt_entry->starting_lba);
			*end_lba = le64_to_cpu(gpt_entry->ending_lba);
			rv = 0;
			goto unlock;
		}
	}
unlock:
	up_read(&serjio_pd->gpt_rwsem);

out:
	NFOUT;
	return rv;
}

int nvmeibs_toma_report_event_serjio_state_change(
	const char *disk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_state)
{
	NFIN;
	BUG_ON(!nvmeibs_get_um_comm());
	nvmeibs_um_comm_serjio_state_changed(nvmeibs_get_um_comm(), disk_id, vendor_id, model_str, serjio_state);
	NFOUT;
	return 0;
}

/******************** Debug accessor functions for simulators *****************/
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)

#include "server/nvmeibs_serjio_sim_access.h"

void nvmeibs_serjio_drain_wq(struct nvmeibs_disk_info *di) {
	struct nvmeibs_serjio_disk_private_data *serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di);
	wq_drain(serjio_pd->io_wq);
}

void nvmeibs_serjio_wait_serjio_ready(struct nvmeibs_disk_info *di) {
	int i;
	struct nvmeibs_serjio_disk_private_data *serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di);
	for (i = 1; !IS_SERJIO_READY(serjio_pd); i++) {
		schedule();
		if ((i & 0xFFFF) == 0) {
			_NEs(error_serjio_nvmeibs_serjio_wait_serjio_ready, serjio_pd, "Serjio initialization stuck=@INT ???", i);	// 1[sec]?
		}
		if (i > 1000000) {
			BUG_ON(1);
		}
	}
	//wq_drain(serjio_pd->io_wq);
}

union jblock_md* nvmeibs_serjio_get_jmdc_ptr(struct nvmeibs_disk_info *di) {
	return get_jmdc_entry(&nvmeibs_disk_info_get_serjio_private_data(di)->jranges_alloc_tbl.ranges[0], 0);
}

int nvmeibs_serjio_find_jrange_index_by_client_uuid(struct nvmeibs_disk_info* di, uuid_be client_uuid) {
	struct nvmeibs_serjio_disk_private_data *serjio_pd = nvmeibs_disk_info_get_serjio_private_data(di);
	struct jranges_allocation_table *jranges_alloc_tbl = &serjio_pd->jranges_alloc_tbl;
	struct jrange_entry *jrange_iter;
	unsigned long flags;
	int found = -ENOENT;

	spin_lock_irqsave(&jranges_alloc_tbl->lock, flags);
	hash_for_each_possible(jranges_alloc_tbl->reserved_ranges,
							   jrange_iter, link, hash_uuid(client_uuid)) {
		if (memcmp(&jrange_iter->client_uuid, &client_uuid, sizeof(client_uuid)) == 0) {
			found = jrange_iter->range_idx;
			break;
		}
	}
	spin_unlock_irqrestore(&jranges_alloc_tbl->lock, flags);
	return found;
}

static int __wait_for_jam_responses_on_jri(struct nvmeibs_serjio_disk_private_data *spd, u32 jri) {
	(void)spd;
	(void)jri;
	return 0;
}

void nvmeibs_serjio_entry_do(struct nvmeibs_disk_info* di, u32 jri, u32 ei, const char *action) {
	struct nvmeibs_serjio_disk_private_data *spd = nvmeibs_disk_info_get_serjio_private_data(di);
	int rv = -1;
	if (action[0] == 'A'){	// Abandon Entry
		rv = set_jrange_entry_state_external(spd, jri, ei, JENTRY_ABND);
	}
	else if (action[0] == 'U'){	//unknown_entry
		rv = set_jrange_entry_state_external(spd, jri, ei, JENTRY_UNKNOWN);
	}
	else if (action[0] == 'D') {	// Drain Communication
		rv = __wait_for_jam_responses_on_jri(spd, jri);
	} else if (action[0] == 'F' || action[0] == 'f') {	// Free Entry
		//__wait_for_jam_responses_on_jri(spd, jri);
		rv = set_jrange_entry_state_external(spd, jri, ei, JENTRY_FREE);
		if (action[0] == 'f'){
			rv = 0;
		}
		//__wait_for_jam_responses_on_jri(spd, jri);
	}
	BUG_ON(rv < 0);										// Illegal to abandon twice or free twice
}

void nvmeibs_serjio_verify_no_abandoned_entries(struct nvmeibs_disk_info* di) {
	struct nvmeibs_serjio_disk_private_data *spd = nvmeibs_disk_info_get_serjio_private_data(di);
	unsigned long flags;
	u32 jri;
	unsigned i;

	spin_lock_irqsave(&spd->jranges_alloc_tbl.lock, flags);
	for (jri = 0; jri < spd->jranges_alloc_tbl.num_ranges; jri++) {
		const struct jrange_entry *je = &spd->jranges_alloc_tbl.ranges[jri];
		for (i = 0; i < je->n_ents; i++){
			BUG_ON(GET_JENTRY_STATE_FROM_BMP(je->jentry_state_bmp, i) == JENTRY_ABND);
			BUG_ON(GET_JENTRY_STATE_FROM_BMP(je->jentry_state_bmp, i) == JENTRY_UNKNOWN);
		}
	}
	spin_unlock_irqrestore(&spd->jranges_alloc_tbl.lock, flags);
}

u32 nvmeibs_serjio_get_abandoned_bmp_by_jri(struct nvmeibs_disk_info* di, u32 jri) {
	struct nvmeibs_serjio_disk_private_data *spd = nvmeibs_disk_info_get_serjio_private_data(di);
	unsigned long flags;
	u32 rv = 0, mask = 0x01;
	int i;
	spin_lock_irqsave(&spd->jranges_alloc_tbl.lock, flags);
	for (i = 0; i < 32; i++, mask <<= 1) {
		if (GET_JENTRY_STATE_FROM_BMP(
				spd->jranges_alloc_tbl.ranges[jri].jentry_state_bmp, i) == JENTRY_ABND) {
			rv |= mask;
		}
	}
	spin_unlock_irqrestore(&spd->jranges_alloc_tbl.lock, flags);
	return rv;

}

void nvmeibs_serjio_get_full_gen_id_info(struct nvmeibs_disk_info* di, u32 jri, u32 ei, struct nvmeibs_serjio_full_gen_id_info *out) {
	struct nvmeibs_serjio_disk_private_data *spd = nvmeibs_disk_info_get_serjio_private_data(di);
	const struct jrange_entry *je = &spd->jranges_alloc_tbl.ranges[jri];

	BUG_ON(!spd);
	BUG_ON(jri >= NVMEIB_EC_MAX_JOURNAL_RANGES);
	BUG_ON(ei >= je->n_ents);
	BUILD_BUG_ON(ARRAY_MEM_SIZE(spd->boot_id) != ARRAY_MEM_SIZE(out->boot_id));

	memcpy(out->boot_id, spd->boot_id, ARRAY_MEM_SIZE(spd->boot_id));
	out->jri_gen_id = spd->jranges_alloc_tbl.ranges[jri].gen_id;
	out->ent_gen_id = spd->jranges_alloc_tbl.ranges[jri].jentry_md[ei].ent_gen_id;
	out->wrapped = spd->jranges_alloc_tbl.ranges[jri].ent_gen_id_wrapped;
}

#if 0		// DEbug function, do ++ on serjio counters to simualte answer from JAM
void nvmeibs_serjio_pp(struct nvmeibs_disk_info* di, u32 jri) {
	struct nvmeibs_serjio_disk_private_data *spd = nvmeibs_disk_info_get_serjio_private_data(di);
	struct jrange_entry *r = &spd->jranges_alloc_tbl.ranges[jri];
	struct jam_update *jam_update = &r->jam_update;
	unsigned long flags;

	spin_lock_irqsave(&spd->jranges_alloc_tbl.lock, flags);
	jam_update->resp_ctr++;
	spin_unlock_irqrestore(&spd->jranges_alloc_tbl.lock, flags);
	//if (jam_update->msg_ctr > jam_update->resp_ctr) {
}
#endif

#endif
