/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_SHARED_EC_H
#define NVMEIB_SHARED_EC_H

/**************************** JMD block 64 bit metadata ***********************/
/* This is the entry in jmd-cache which resides in srv's memory. This is also the first 8 bytes of a journal-block's MD buffer. */
#define NVMEIB_EC_JMDC_BITS_J2D	        (32) // The number of bits in MD to maintain pointer from the journal block to the dlba
#define NVMEIB_EC_JMDC_BITS_TX_ID		(20) // The number of bits in MD for transaction ID.

#define NVMEIB_EC_JMDC_BITS_TX_BMP	    10 // Todo: Reduce to 10, The number of bits in MD for transaction member bitmap, Optimization: This number can be less bits for smaller slice_size and can be dynamically calculated.
#define NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP   8 // Same as above in compressed version
#define NVMEIB_EC_JMDC_BITS_VER          2
#define NVMEIB_EC_JMDC_BITS_LINK         1 // Link bit for chain of journals

/* This is the entry in jmd-cache which resides in srv's memory.
   This is also the first 8 bytes of a journal-block's MD buffer. */
union jblock_md {
	struct {
		u32 j2d_0		                : NVMEIB_EC_JMDC_BITS_J2D; 			// journal -> dat pointer. Unsafe to use directly!
		u32 tx_id		                : NVMEIB_EC_JMDC_BITS_TX_ID;	  	// transaction id
		u32 tx_bmp		                : NVMEIB_EC_JMDC_BITS_TX_BMP;		// transaction bitmap, , i.e.: which segs are written in transaction, excluding parities, relative to slice start (not to raid first segment)
		u32 version		                : NVMEIB_EC_JMDC_BITS_VER;			// Pre multi-slice implementation, always 0
	} __attribute__((packed));
	struct {
		u32 j2d_1	 	                : NVMEIB_EC_JMDC_BITS_J2D; 			// journal -> dat pointer. Unsafe to use directly!
		u32 tx_id		                : NVMEIB_EC_JMDC_BITS_TX_ID;	  	// transaction id
		u32 tx_bmp_zip	                : NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP;	// transaction bitmap, , i.e.: which segs are written in transaction, excluding parities, relative to slice start (not to raid first segment)
		u32 has_next                    : NVMEIB_EC_JMDC_BITS_LINK;
		//u32 has_prev                    : NVMEIB_EC_JMDC_BITS_LINK;
		u32 j2d_extended                : NVMEIB_EC_JMDC_BITS_LINK;			// Support segment size of 2^33[blks] = 32[TB]. Without it j2d supports only up to 16[TB] segments
		u32 version		                : NVMEIB_EC_JMDC_BITS_VER;			// With multi-slice implementation, is 1
	} v1 __attribute__((packed));
	u64 raw;												// Size of journal entry meta-data (Uses the minimum to support drives that only support 4104 format)
}__attribute__((packed));
#define jblock_md_prev_link(md) (0)							// Field was removed: ((md)->v1.has_prev)

#define NVMEIBC_JOURNAL_MD_VERSION_UNPACKED  (0)		// 2.0.4 and before version. Unpacked txbm, without notion of multi-slice journal
#define NVMEIBC_JOURNAL_MD_VERSION_PACKED    (1)		// Packed TxBM, with notion of multi-slice journal + support for 32[TB] drives
static inline u64 __dp_ec_jmd_get_u64_j2d(const union jblock_md *md)
{
	return ((u64)md->v1.j2d_1 | (((u64)md->v1.j2d_extended) << 32));
}

static inline u64 nvmeibc_block_dp_ec_jmd_decode_j2d_only(const union jblock_md *md)
{
	return (md->version == NVMEIBC_JOURNAL_MD_VERSION_PACKED) ? __dp_ec_jmd_get_u64_j2d(md) : (md)->j2d_0;
}

static inline void nvmeibc_block_dp_ec_jmd_encode_j2d_only(union jblock_md *md, u64 j2d)
{
	if (md->version == NVMEIBC_JOURNAL_MD_VERSION_PACKED) {
		md->v1.j2d_1 = (u32)(j2d);
		md->v1.j2d_extended = (u32)(j2d >> 32);
	} else {
		md->j2d_0 = (u32)j2d;
	}
}

static inline void jblock_md_jmd_encode_v0(union jblock_md *md, u32 j2d, u32 tx_id, roles_bmp_t tx_bmp)
{
	md->j2d_0 = (u32)j2d;
	md->tx_id = tx_id;
	md->tx_bmp = tx_bmp;
	md->version = NVMEIBC_JOURNAL_MD_VERSION_UNPACKED;	// Compatible with previous versions
}

/*** TxID Life Cycle ***
1) After format, the whole metadata may be 0 or -1 and we don't really have any control over it.
   Both cases are considered as never written, before ever descending into looking at specific
   fields in metadata.
2) Values of TxID = 0 (INITIAL_LAZY_READ_TXID) or TxID = 1 (NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS)
   are never explicitly written to disk to avoid confusion.
3) Good path: Initial value of TxID in ram is NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS = 1.
   After first write it will be 2. When reaching TxID > NVMEIBC_DP_EC_MD_TX_ID_MAX, wraparound occurs.
   After wraparound, TxID=1 (NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS) is written to indicate metadata was
   reset and there are no journals.
4) Special values of TxID:
   * INITIAL_LAZY_READ_TXID = 0 : Lazy read, meaning toma lost the previous value of TxID.
     Can appear only in ram. If during io this value is encountered, TxID resolve occurs.
   * NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS = 1 : No journals. Initial value of TxID in ram.
     This will be the value of TxID after wraparound. If cold recovery reads a block with TxID = 1,
     it is an indication to that it is after wraparound (or other special case below) and has no journal.
     It can also indicate a special metadata marker. When TxID = 1 is read, and version is valid, JRI is considered:
        JRI = -2 : Indication of block invalid for read. Read operations on this block should fail.
        JRI = 0 : The usual value after wraparound. Good path.
        JRI = 2 : Block explicitly marked as if it was never written to ignore edic check.
		          This may be marked in special case of write hole fix.
5) Double dead parities - TxID will keep growning (and wraparound) but no journals are written.
*/

#define NVMEIBC_DP_EC_MD_TX_ID_UNSET		(0)	// 0 = Never wrtitten to disk, but may be read by accident if reading uninitialized block
#define INITIAL_LAZY_READ_TXID				(0)	// 0 = Unknown TxID when appears in binfo (in RAM). The initial TXID in RAM if seg is non RW. Need to replace with the max TXID from one of the parity blksets (128k), on first write
#define NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS  (1)	/* 1 = Initial TxID and unrecoverable as well: post cold recovery / praid init / TxID wraparound */
#define NVMEIBC_DP_EC_MD_TX_ID_MAX			((1U << NVMEIB_EC_JMDC_BITS_TX_ID) - 2)	// Max Transaction ID value to be written on disk
#define NVMEIBC_DP_EC_MD_TXBM_INVALID       (0) // 0 = jmd is not in use.
#define NVMEIBC_DP_EC_MD_N_RESERVED_TXBM_VALUES (1)  // Just NVMEIBC_DP_EC_MD_TXBM_INVALID

#define nvmeib_txid_never_write_to_disk(txid) ((txid > NVMEIBC_DP_EC_MD_TX_ID_MAX) || (txid == NVMEIBC_DP_EC_MD_TX_ID_UNSET))		// true if we may have journal for given txid false otherwise
#define nvmeib_txid_no_journal(txid) ((txid == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS) || (txid == NVMEIBC_DP_EC_MD_TX_ID_UNSET))		// true if we may have journal for given txid false otherwise
// JMDC entry not after format or explicitly set to invalid
static inline bool nvmeib_is_jmd_io_entry_by_txid(const u32 tx_id)
{
    if ((tx_id == INITIAL_LAZY_READ_TXID)||(tx_id == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS)||(tx_id > NVMEIBC_DP_EC_MD_TX_ID_MAX))
        return false;
    return true;
}

static inline bool _nvmeib_is_jmd_io_entry(const union jblock_md *md)
{
    if (!nvmeib_is_jmd_io_entry_by_txid(md->tx_id))
        return false;
    if (md->tx_bmp == NVMEIBC_DP_EC_MD_TXBM_INVALID)
        return false;
    return true;
}

/* Value written to JMD/JMDC when a journal entry is in free state */
static const union jblock_md nvmeib_jmd_unused_entry_val = {{
		.j2d_0 =   (~(0U)),							// All 1's, meaningless value, just for backwards compatibility
		.tx_id =   NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS,
		.tx_bmp =  NVMEIBC_DP_EC_MD_TXBM_INVALID,
		.version = NVMEIBC_JOURNAL_MD_VERSION_UNPACKED,
}};

/* Value used to indicate JMD/JMDC value is not relevant/available */
static const union jblock_md nvmeib_jmd_invalid_special_val = {{
		.j2d_0 =   (~(0U)),							// All 1's, meaningless value, just for backwards compatibility
		.tx_id =   NVMEIBC_DP_EC_MD_TX_ID_MAX + 1, // This is the only difference from invalid value
		.tx_bmp =  NVMEIBC_DP_EC_MD_TXBM_INVALID,
		.version = NVMEIBC_JOURNAL_MD_VERSION_UNPACKED,
}};

/* Value used to indicate JMD block not used in transaction [sw_lba - 1 .. binje - 1 ] */
static const union jblock_md nvmeib_jmd_unused_jblock_val = {
		.v1 = {
			.j2d_1 =   (~(0U)),							// All 1's, meaningless value, just for backwards compatibility
			.tx_id =   NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS,
			.tx_bmp_zip =  NVMEIBC_DP_EC_MD_TXBM_INVALID,
			.has_next = 0,
			.j2d_extended = 0,
			.version = NVMEIBC_JOURNAL_MD_VERSION_PACKED,
		},
};

static const union jblock_md nvmeib_jmd_trim0_jblock_val = {
		.raw = 0,
};

static const union jblock_md nvmeib_jmd_trim1_jblock_val = {
		.raw = ~(u64)0,
};

#define nvmeib_is_jmd_io_entry(jmdc_u64_val)            _nvmeib_is_jmd_io_entry((const union jblock_md *)&(jmdc_u64_val))
#define nvmeib_is_jmd_invalid_special(jblock_md_ptr) ((jblock_md_ptr)->raw == nvmeib_jmd_invalid_special_val.raw)
#define nvmeib_is_jmd_unused_entry(   jblock_md_ptr) ((jblock_md_ptr)->raw == nvmeib_jmd_unused_entry_val.raw)
#define nvmeib_is_jmd_unused_jblock(   jblock_md_ptr) ((jblock_md_ptr)->raw == nvmeib_jmd_unused_jblock_val.raw)
#define nvmeib_is_jmd_trim_val(jblock_md_ptr)			((jblock_md_ptr)->raw == nvmeib_jmd_trim0_jblock_val.raw || (jblock_md_ptr)->raw == nvmeib_jmd_trim1_jblock_val.raw)

/****************************** jentry ****************************************/
struct jentry_md {	 							// Just a wrapper to assist the programmer to differntiate between pointer to jblock and jentry (array of jblocks)
	union jblock_md *md_arr;
}__attribute__((packed));

static inline bool nvmeib_jentry_md_is_unused_fast(		const struct jentry_md *jentry, const binje_t binje)					// Todo: Unite 2 functions below
{
	(void)binje; return nvmeib_is_jmd_unused_entry(&jentry->md_arr[0]);
}
static inline bool nvmeib_jentry_md_is_unused_slow(		const union jblock_md *jentry,  const binje_t binje)					// Serjio debug version of the above function, that verifies all blocks, not jsut the first one
{
	unsigned i;
	for (i = 0; i < binje; i++) {
		if (jentry[i].raw != nvmeib_jmd_unused_entry_val.raw)
			return false;
	}
	return true;
}

bool nvmeib_jentry_md_is_valid(							const struct jentry_md *jentry, const binje_t binje);								// Currently implementation exists in client only. Todo, move it common code
static inline bool nvmeib_jentry_md_is_invalid_special(	const struct jentry_md *jentry,       binje_t binje)
{
	(void)binje; return nvmeib_is_jmd_invalid_special(&jentry->md_arr[0]);
}
#if 0			// Not implemented yet
union jam_hkey * nvmeib_jentry_md_get_nonambig_jam_key(	const struct jentry_md *jentry,       binje_t binje);
union jblock_md * nvmeib_jentry_md_get_dlbas(			const struct jentry_md *jentry,       binje_t binje, u64 *slba, u64 *elba);
#endif
static inline size_t nvmeib_shared_set_jentry_md_unused(union jblock_md *jmd, const binje_t binje)
{
	unsigned i;
	for (i = 0; i < binje; i++)
		jmd[i].raw = nvmeib_jmd_unused_entry_val.raw;
	return (sizeof(jmd[0])*binje);
}

static inline size_t nvmeib_shared_set_jentry_md_invalid_special(union jblock_md *jmd, const binje_t binje)
{
	unsigned i;
	for (i = 0; i < binje; i++)
		jmd[i].raw = nvmeib_jmd_invalid_special_val.raw;
	return (sizeof(jmd[0])*binje);
}

/**************************** jentry_md_container *****************************/
struct jentry_md_container {
	union jblock_md jblks_md[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];
}__attribute__((packed));

void nvmeibc_jentry_md_container_fill(struct jentry_md_container *dst, struct jentry_md *src, u32 sw2hw, u32 n, binje_t binje);

static inline unsigned nvmeib_ec_journal_entries_in_range_binje(binje_t binje, unsigned blk_in_rng)
{
	unsigned ent_in_rng = blk_in_rng / binje;
	if (NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE < ent_in_rng)
		return NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
	return ent_in_rng;
}

#endif //_NVMEIB_SHARED_EC

