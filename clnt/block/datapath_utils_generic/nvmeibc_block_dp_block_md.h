#ifndef NVMEIBC_BLOCK_DP_BLOCK_MD_H
#define NVMEIBC_BLOCK_DP_BLOCK_MD_H
#include "common/kr_incs.h"

/* EC Block Meta Data
 * The below objects represent the data we attach to each data block, as
 * meta-data. This meta-data is firstly used to enable Erasure Coding &
 * more advanced features like T10 DIF.
 * We start with MD stored within the disk MD area. MD size is 8B (4096+8)
 */

// Data block MD field sizes
#define NVMEIBC_DP_EC_DMD_BITS_VERSION	 2		// The number of bits in MD for layout version

/* Special markers in metadata that mark validity state of data. They mark it in
   JRI and valid only with conjunction of NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS */
enum nvmeibc_md_validity_e {
	JRI_MARK_NO_JOURNAL = 0,									// Default value to metadata which is written without journal (reconstruction using parity). Cold/HTR will treat it as commited data if comes with the right txid.
	JRI_MARK_INVALID_FOR_READ = 1,								// Logical bad sector: Slice was intentionally destroyed by slice failed recovery (more than p segments are down).
	JRI_MARK_DATA_INVALID_IS_ZEROS = NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES - 1, // == 2. Data and metadata was never written by block device (Result of wiping the disk before allocating volumes). Safefly say that data is 0, even though it does not match the 0 EDIC
	NVMEIBC_MIRROR_UNUSED_TXID_JRI = 6, 						// Txid and jri are unused in mirror, hence we will always set them to these reserved values
};

#define DEBUG_SAVE_JENTRY 1
/* No longer dependent on DEBUG_SAVE_JENTRY for compatibility reasons. To change, must increase DMD version */
#define NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS (9)

#define DB_DESCRIPTOR_BITS (4) // ceil(log2(N_MAX_RAID_SLICE_LEN))

// EDIC bits are what ever bits remain after storing others
#define NVMEIBC_DP_EC_DMD_BITS_EDIC	(32 - NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS - NVMEIBC_DP_EC_DMD_BITS_VERSION)
#define NVMEIBC_DP_EC_PMD_BITS_EDIC	(NVMEIBC_DP_EC_DMD_BITS_EDIC - 2*DB_DESCRIPTOR_BITS)

/* No longer dependent on journal macros for compatibility reasons. To change, must increase DMD version */
#define NVMEIBC_DP_EC_DMD_BITS_JCI	(10)

#define NVMEIBC_DP_EC_MD_EDIC_MASK(is_p) \
	((u32)((1U << ((is_p) ? NVMEIBC_DP_EC_PMD_BITS_EDIC : NVMEIBC_DP_EC_DMD_BITS_EDIC)) - 1))

#define NVMEIBC_DP_EC_QLC_MD_EDIC_MASK ((u32)(~0U))

// The MD version (attached to data blocks) issued by newly compiled code.
#define NVMEIBC_DATA_MD_VERSION		1
#define RESERVED_MD_BITS	(32 - NVMEIB_EC_JMDC_BITS_TX_ID - NVMEIBC_DP_EC_DMD_BITS_JCI)
// Data block MD
union nvmeibc_block_dp_ec_data_block_md {					// Minimal size of 8 bytes struct. Actual metadata entry may be bigger
	struct {												// Encapsulate
		union {												// First 32 bits
			struct {
				u32	version	: NVMEIBC_DP_EC_DMD_BITS_VERSION;	// MD object version
#ifdef DEBUG_SAVE_JENTRY
				u32 d2j_rng : NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS;		// Compare candidates and found entries with exact D2J for debug
#else
				u32 d2j_rsvd: NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS;
#endif
				u32	edic	: NVMEIBC_DP_EC_DMD_BITS_EDIC;		// End to End Data Integrity Check for Data block
			} __attribute__((packed)) D;
			struct {
				u32	version	: NVMEIBC_DP_EC_DMD_BITS_VERSION;	// MD object version
#ifdef DEBUG_SAVE_JENTRY
				u32 d2j_rng : NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS;		// Compare candidates and found entries with exact D2J for debug
#else
				u32 d2j_rsvd: NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS;
#endif
				u32	edic	: NVMEIBC_DP_EC_PMD_BITS_EDIC;		// End to End Data Integrity Check for parity blocks
				u32	dbits_0	:  DB_DESCRIPTOR_BITS;				// Post transaction Dbits encoded in parity blocks
				u32	dbits_1	:  DB_DESCRIPTOR_BITS;
			} __attribute__((packed)) P;
		} __attribute__((packed));
		u32		tx_id	: NVMEIB_EC_JMDC_BITS_TX_ID;			// Transacion ID
		u32		jri		: NVMEIBC_DP_EC_DMD_BITS_JCI;			// Journal chunk/range index JCI if @tx_id!=NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS, o/w see enum nvmeibc_dp_ec_data_block_md_jri_overload
		u32		reserved: RESERVED_MD_BITS;						// Reserved 2 bits
	} __attribute__((packed));
	u64 raw;
} __attribute__((packed));

static const union nvmeibc_block_dp_ec_data_block_md nvmeibc_ec_unwritten_md_entry_val = { .raw = ~0ULL };	// Initial values of MD after format are all 1's and data are 0's.
static const union nvmeibc_block_dp_ec_data_block_md nvmeibc_ec_backdoor__md_entry_val = { .raw =  0ULL };	// When writing with 'dd' during manual fix of slice, directly to backdoor of disk the data is correct but metadata is 0 (incorrect), because 'dd' does nto write metadata
#define NVMEIBC_DATA_MD_MIRROR_IS_PARITY (true)																// All legs of raid1 with metadata act as parity (with regard to dirty bits)
static inline void nvmeibc_block_dp_ec_md_set_edic(union nvmeibc_block_dp_ec_data_block_md *md, u32 edic, bool is_p)
{
	if (is_p) md->P.edic = (edic);
	else      md->D.edic = (edic);
}

static inline u32 nvmeibc_block_dp_ec_md_get_edic(const union nvmeibc_block_dp_ec_data_block_md *md, bool is_p)
{
	return (is_p) ? md->P.edic : md->D.edic;
}

static inline void nbdpec_md_mark_valid_version(union nvmeibc_block_dp_ec_data_block_md *md) {
	md->D.version = NVMEIBC_DATA_MD_VERSION;		// Same as: md->P.version = NVMEIBC_DATA_MD_VERSION;
}

void nvmeibc_block_dp_ec_copy_md_dbits(const union nvmeibc_block_dp_ec_data_block_md *src, union nvmeibc_block_dp_ec_data_block_md *dst);
void nvmeibc_block_dp_ec_copy_md(      const union nvmeibc_block_dp_ec_data_block_md *src, union nvmeibc_block_dp_ec_data_block_md *dst);

static inline void nvmeibc_dbits_entry_get_ind(const union nvmeibc_dbits_entry *e, int *d_0, int *d_1)
{
	if (nvmeibc_dbits_entry_is_global_mode(e)) {			// Must not contain unknowns, loose convictness info here
		WARN(nvmeibc_dbits_has_unknowns(e), "nvmeibc bug! wrong usage=%d\n",e->all_bits);
		*d_0 = e->bsmod.dead0;
		*d_1 = e->bsmod.dead1;
	} else {
		*d_0 = e->slmod.dead0;
		*d_1 = 0;
	}
	WARN((*d_0 > N_MAX_RAID_SLICE_LEN)||(*d_1 > N_MAX_RAID_SLICE_LEN), "nvmeibc bug! illegal dbit={0x%x,0x%x} ent=0x%x", (*d_0), (*d_1), e->all_bits);		// Traps for un/initialized MD values
}

static inline void nvmeibc_block_dp_ec_md_fill_p_with_dbits(union nvmeibc_block_dp_ec_data_block_md *md, const union nvmeibc_dbits_entry dbits)
{
	int d0 = 0, d1 = 0;
	nvmeibc_dbits_entry_get_ind(&dbits, &d0, &d1);
	md->P.dbits_0 = d0;
	md->P.dbits_1 = d1;
}

// initialize all bit-fields of the data-block meta-data
static inline void nvmeibc_block_dp_ec_md_make_d(union nvmeibc_block_dp_ec_data_block_md *md, u32 edic, u32 jri, u32 tx_id, __attribute__ ((unused)) u32 d2j_rng)
{
	nbdpec_md_mark_valid_version(md);
	md->D.edic = edic;
	#ifdef DEBUG_SAVE_JENTRY
		md->D.d2j_rng = d2j_rng;
	#endif
	md->tx_id = tx_id;
	md->jri	= jri;
}

static inline void nvmeibc_block_dp_ec_md_make_p(union nvmeibc_block_dp_ec_data_block_md *md, u32 edic, u32 jri, u32 tx_id, const union nvmeibc_dbits_entry dbits, __attribute__ ((unused)) u32 d2j_rng)
{
	nvmeibc_block_dp_ec_md_make_d(md, edic, jri, tx_id, d2j_rng);
	nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, dbits);				// Overwrites part of md->P.edic
}

// initialize all bit-fields of the raid1-block
static inline void nvmeibc_block_dp_ec_md_make_r1(union nvmeibc_block_dp_ec_data_block_md *md, u32 edic)
{
	nbdpec_md_mark_valid_version(md);
	BUILD_BUG_ON(!NVMEIBC_DATA_MD_MIRROR_IS_PARITY);	// Not supported yet. All legs of raid-1 currently write as parity, but without dbits
	md->P.edic = edic;		// Short edic, in future we might use dbits as well
	md->tx_id = NVMEIBC_MIRROR_UNUSED_TXID_JRI;
	md->jri =   NVMEIBC_MIRROR_UNUSED_TXID_JRI;
}

static inline bool nbdpec_md_was_data_explicitly_marked_never_written(const union nvmeibc_block_dp_ec_data_block_md *md)
{
	return ((md->tx_id == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS) && (md->jri == JRI_MARK_DATA_INVALID_IS_ZEROS));
}

static inline bool is_data_invalid_for_read(const union nvmeibc_block_dp_ec_data_block_md *md)
{
	return (md->jri == JRI_MARK_INVALID_FOR_READ);
}

enum nvmeibc_data_written_state {					// Metadata definition for 4KB block state. Note: When reading blocks directly from disk through backdoor -> this will return the 4KB block regardless of what metadata state says.
	DATA_VIRGIN = 1,								// Fresh formatted disk / data written with dd to backdoor after manual fix of slice. Metadata is incorrect but data is correct
	DATA_EXPLICITLY_MARKED_NEVERWRITTEN = 2,		// Regardless of what is in 4KB block, treat it as zeros for parity calculation and user read
	DATA_EXPLICITLY_MARKED_INVALID = 3,				// Regardless of what is in 4KB block, treat it as bad sector.
	DATA_WRITTEN = 4,								// Normally written data
};

static inline char *nvmeibc_data_written_state_tostring(enum nvmeibc_data_written_state state)
{
	switch (state) {
		case DATA_VIRGIN:							return "Virg";	// Not written via datapath (either fresh format or external write, like 'dd')
		case DATA_EXPLICITLY_MARKED_NEVERWRITTEN:	return "ZERO";	// Marked as zeros
		case DATA_EXPLICITLY_MARKED_INVALID:		return "BAD!";	// Bad sector
		case DATA_WRITTEN:							return "_OK_";
		default:									return "????";
	}
}

enum metadata_action {								// When reading block, covnert metadata state of block to action. This enum consolidates different metadata formats EC/QLC/R1 into action during read
	RESUME = 0,										// No special action, just continue with normal read
	SKIP_EDIC_CHECK = 1,
	BAD_SECTOR = 2,									// Return error
	MARK_NEVER_WRITTEN = 3							// Alter metadata state from broken/invalid to never written marker
};

/* Check if data was never written to, otherwise edic calculation is required */
static inline enum nvmeibc_data_written_state nbdpec_md_get_data_written_state(const union nvmeibc_block_dp_ec_data_block_md *md)
{
	if (md->raw == nvmeibc_ec_backdoor__md_entry_val.raw) return DATA_VIRGIN;
	if (md->raw == nvmeibc_ec_unwritten_md_entry_val.raw) return DATA_VIRGIN;
	if (md->D.version == NVMEIBC_DATA_MD_VERSION) { // My Version
		if (nbdpec_md_was_data_explicitly_marked_never_written(md))
			return DATA_EXPLICITLY_MARKED_NEVERWRITTEN;
		if (is_data_invalid_for_read(md))
			return DATA_EXPLICITLY_MARKED_INVALID;
	} else {    // Trap for unknown versions in MD
		WARN(true, "nvmeib bug! unknown md version=%u, md=%llx", md->D.version,  (long long int)md->raw);
	}
	return DATA_WRITTEN;
}

/* Check if data was never written to, otherwise edic calculation is required */
static inline bool nbdpec_md_was_data_never_written(const union nvmeibc_block_dp_ec_data_block_md *md)
{
	return (nbdpec_md_get_data_written_state(md) < DATA_EXPLICITLY_MARKED_INVALID); // If we find such a MD we re-mark it as never written with our newer version
}

static inline union nvmeibc_dbits_entry fill_nvmeibc_dbits_entry_from_md(const union nvmeibc_block_dp_ec_data_block_md *smd)
{
	union nvmeibc_dbits_entry dst = {.all_bits = 0};		// Default No Dbits
	if (nbdpec_md_get_data_written_state(smd) == DATA_VIRGIN) // Virgin parity doesn't have dbits.
		return dst;

	WARN((smd->P.dbits_0 > N_MAX_RAID_SLICE_LEN)||(smd->P.dbits_1 > N_MAX_RAID_SLICE_LEN), "nvmeibc bug! illegal dbit={0x%x,0x%x} md=0x%llx", smd->P.dbits_0, smd->P.dbits_1, (long long int) smd->raw);		// Traps for un/initialized MD values
	if (smd->P.dbits_1) { 		// No convict in MD
		dst.bsmod.dead0 = smd->P.dbits_0;
		dst.bsmod.dead1 = smd->P.dbits_1;
	} else if (smd->P.dbits_0) { // single degraded
		dst.slmod.dead0 = smd->P.dbits_0; // Todo: What about slice dirty info
	}
	return dst;
}

typedef u8 roles_pair_compressed_t; // Bitmaps which are used to represent segments groups,   first bit is seg0, last bit is last Seg
typedef roles_pair_compressed_t dbits_compressed_t;		 // Bitmaps which are used to represent segments groups,   first bit is seg0, last bit is last Seg
typedef roles_pair_compressed_t txbm_compressed_t;		 // Bitmaps which are used to represent segments groups,   first bit is seg0, last bit is last Seg

void init_roles_pair_compression_tables(void);   // init compression table

struct decomp_entry {
	u8 l, h;
} __attribute__((packed));

static inline roles_pair_compressed_t nvmeibc_block_dp_ec_md_roles_pair_compress(const raid_role_t l, const raid_role_t h)
{
	extern roles_pair_compressed_t roles_comp_table[N_MAX_RAID_SLICE_LEN][N_MAX_RAID_SLICE_LEN];
	return roles_comp_table[l][h];
}

static inline void nvmeibc_block_dp_ec_md_roles_pair_decompress(const roles_pair_compressed_t z, raid_role_t *l, raid_role_t *h)
{
	extern struct decomp_entry roles_decomp_table[];
	struct decomp_entry *res = &roles_decomp_table[z];
	WARN((res->l > N_MAX_RAID_SLICE_LEN) || (res->h > N_MAX_RAID_SLICE_LEN), "illegal decompressed value, l=%d h=%d compressed_val=%u\n", res->l, res->h, z);
	(*l) = res->l;
	(*h) = res->h;
	return;
}

static inline txbm_compressed_t nvmeibc_block_dp_ec_md_txbm_compress(const roles_bmp_t tx_bmp)
{
	if (tx_bmp == NVMEIBC_DP_EC_MD_TXBM_INVALID) { // unused compressed value stays 0
		return NVMEIBC_DP_EC_MD_TXBM_INVALID;
	} else {
		const raid_role_t start = __builtin_ctzl(tx_bmp);
		const raid_role_t end = (sizeof(unsigned long) * 8) - __builtin_clzl(tx_bmp) - 1;
		return nvmeibc_block_dp_ec_md_roles_pair_compress(start, end);
	}
}

static inline roles_bmp_t nvmeibc_block_dp_ec_md_txbm_decompress(const txbm_compressed_t tx_bmp)
{
	raid_role_t l, h;
	if (tx_bmp == NVMEIBC_DP_EC_MD_TXBM_INVALID) { // unused compressed value stays 0
		return NVMEIBC_DP_EC_MD_TXBM_INVALID;
	}
	nvmeibc_block_dp_ec_md_roles_pair_decompress((roles_pair_compressed_t)tx_bmp, &l, &h);
	return GENMASK(h, l);
}

/* Important: For internal use only don't write to disk. Generic form, regardless of compression and various versions */
struct jblock_md_decompressed {
	u64 j2d;
	u32 tx_id;
	roles_bmp_t tx_bmp;
	u32 version : 8;					// For debug/print only: not really needed, store what was the original version of the decoded jblock_md.
	bool has_next;
}__attribute__((packed));

struct jblock_md_decompressed_for_seg {	// Exactly identical to the struct above , just decompressed for specific segment so j2d converted to j2slba
	u64 j2slba;
	u32 tx_id;
	roles_bmp_t tx_bmp;
	u32 version : 8;					// For debug/print only: not really needed, store what was the original version of the decoded jblock_md.
	bool has_next;
}__attribute__((packed));

void nvmeibc_block_dp_ec_jmd_encode(union jblock_md *md, u64 j2d, u32 tx_id, roles_bmp_t tx_bmp, bool has_next, bool has_prev);	// initialize journal-block meta-data
static inline struct jblock_md_decompressed nvmeibc_block_dp_ec_jmd_decode(const union jblock_md *md) {
	struct jblock_md_decompressed res = {0};
	res.tx_id = md->tx_id;
	res.version = md->version;
	res.j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(md);
	if (md->version == NVMEIBC_JOURNAL_MD_VERSION_PACKED) {
		res.tx_bmp = nvmeibc_block_dp_ec_md_txbm_decompress(md->v1.tx_bmp_zip);
		res.has_next = md->v1.has_next;
	} else {
		res.tx_bmp = md->tx_bmp;
		res.has_next = false;
	}
	return res;
}

#define JENT_UNINITIALIZED_OFFSET (u16)(-1)

struct jent_md_decompressed {
	struct jblock_md_decompressed_for_seg md_arr[NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY];
	int len; // The len of md_arr of the specific candidate.
	u16 jent_idx; // jentry index inside its JRI.
	u16 offset; // the first slice in transaction contains this jentry in its txbm (0 for parities for example).
	bool is_valid;  // is the jentry on this seg is committed.
};

/* Assumptions: 1. Chain is valid, verified by nvmeib_jentry_md_is_valid(). 2. res has allocated md_arr with multi_slice_N items*/
void nvmeibc_block_dp_ec_md_decode_jentry(u64 seg_start_dlba, struct jent_md_decompressed *res, const struct jentry_md *ent);

/* Debug, Print array of blocks metadata to string: type = {D,P,J}*/
void nbdpec_md_to_string(void* md_arr, int md_size, int len, char type_DPJ);
static inline int nbdpec_md_to_string_buf(const void* _md, char type, char rv[256]) {
	if (type == 'J') {
		const union jblock_md *md = _md;
		const struct jblock_md_decompressed jmd = nvmeibc_block_dp_ec_jmd_decode(md);
		return snprintf(rv, 256, "{j2d=0x%llx, txid=0x%x, version=0x%x, tx_bmp=0x%x, has_next=0x%x}, 0x%016llx", jmd.j2d, jmd.tx_id, jmd.version, jmd.tx_bmp, jmd.has_next, md->raw);
	} else {
		const union nvmeibc_block_dp_ec_data_block_md *md = _md;
		const enum nvmeibc_data_written_state ws = nbdpec_md_get_data_written_state(md);
		const char *st_str = nvmeibc_data_written_state_tostring(ws);
		if (type == 'D')
			return snprintf(rv, 256, "%s-{ver=%d, d2j_rng=0x%x edic=0x%x, txid=0x%x, jri=%x, rsv=0x%x}, 0x%016llx", st_str, md->D.version, md->D.d2j_rng, md->D.edic, md->tx_id, md->jri, md->reserved, md->raw);
		else if (type == 'P')
			return snprintf(rv, 256, "%s-{ver=%d, d2j_rng=0x%x edic=0x%x, db={%u,%u}, txid=0x%x, jri=%x, rsv=0x%x}, 0x%016llx", st_str, md->D.version, md->D.d2j_rng, md->P.edic, md->P.dbits_0, md->P.dbits_1, md->tx_id, md->jri, md->reserved, md->raw);
	}
	return 0;
}

static inline void nvmeibc_block_dp_ec_md_clean_reserved(union nvmeibc_block_dp_ec_data_block_md *md)
{ // Clean unneeded fields
	#ifdef DEBUG_SAVE_JENTRY
		md->P.d2j_rng = 0;
	#endif
	md->reserved = 0;
}

// Used to ensure we will consider this block never written to in our current version
// NOTE!!! Currently D/P version is in the same place, if changed, is_parity is a must
// A neverwritten can have dirty-bit on it when doing a rollback,
// An example of when can this situation appear is when in EC cold recovery we have dead D0 role and we suspect that someone have been written there something but couldn't make it to the parities so they are still never-written.
static inline void nbdpec_md_mark_data_never_written_with_dbits(union nvmeibc_block_dp_ec_data_block_md *md, __attribute__ ((unused)) const bool is_parity, const union nvmeibc_dbits_entry dbits)
{
	nbdpec_md_mark_valid_version(md);
	md->tx_id = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;
	md->jri = JRI_MARK_DATA_INVALID_IS_ZEROS;
	if (is_parity) {
		nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, dbits);
	} else {
		WARN(dbits.all_bits != 0, "Trying to set dbits to data block.\n");
	}
	nvmeibc_block_dp_ec_md_clean_reserved(md);
}

static inline void nbdpec_md_mark_no_journal(union nvmeibc_block_dp_ec_data_block_md *md, const u32 txid, __attribute__ ((unused)) const bool is_parity)
{
	nbdpec_md_mark_valid_version(md);
	md->tx_id = txid;
	md->jri = JRI_MARK_NO_JOURNAL;
}

/* Rebuild from Data blocks. Edic was already calculated */
static inline void nvmeibc_block_dp_ec_md_set_txid_and_dirty_bits_value_mark_no_journal(union nvmeibc_block_dp_ec_data_block_md *md,
														  u32 txid, const union nvmeibc_dbits_entry dbits)
{
	nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, dbits);
	nbdpec_md_mark_no_journal(md, txid, true);
}

static inline bool nbdpec_md_is_no_journal(union nvmeibc_block_dp_ec_data_block_md *md)
{
	return (md->jri == JRI_MARK_NO_JOURNAL);
}

static inline void nbdpec_md_mark_data_invalid_for_read(union nvmeibc_block_dp_ec_data_block_md *md, __attribute__ ((unused)) const bool is_parity, const u32 txid)
{
	nbdpec_md_mark_valid_version(md);
	md->tx_id = txid;
	md->jri =   JRI_MARK_INVALID_FOR_READ;
}

// Used to ensure we will consider this block never written to in our current version
static inline void nbdpec_md_mark_data_never_written_no_dbits(union nvmeibc_block_dp_ec_data_block_md *md, const bool is_parity)
{
	nbdpec_md_mark_data_never_written_with_dbits(md, is_parity, zero_dbits);
}

static inline void nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_ec(union nvmeibc_block_dp_ec_data_block_md *md, u32 max_txid_in_slice, const bool is_parity, const u32 edic, const union nvmeibc_dbits_entry *dbits)
{ // Externally mark corrupted EC-blocks metadata as writtern, Also removing bad sector / never-written markers if existed
	nvmeibc_block_dp_ec_md_set_edic(md, edic, is_parity);
	if (is_parity) {
		nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, (dbits ? *dbits : zero_dbits));
	}
	nbdpec_md_mark_no_journal(md, max_txid_in_slice, is_parity);
	nvmeibc_block_dp_ec_md_clean_reserved(md);
}

static inline void nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_ec(union nvmeibc_block_dp_ec_data_block_md *md, u32 max_txid_in_slice, const bool is_parity, const u32 edic, bool is_slice_neverwritten, const union nvmeibc_dbits_entry *dbits)
{ // Function is used to fix corrupted EC-blocks metadata externally
	if (is_slice_neverwritten) {
		md->raw = nvmeibc_ec_unwritten_md_entry_val.raw; // Just put in the md neverwritten
	} else if (nbdpec_md_was_data_never_written(md)) {
		nbdpec_md_mark_data_never_written_no_dbits(md, is_parity);
	} else {	// Note: if (is_data_invalid_for_read(md)) << -- Removing the bad sector
		nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_ec(md, max_txid_in_slice, is_parity, edic, dbits);
	}
}

static inline void nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_r1(union nvmeibc_block_dp_ec_data_block_md *md, const u32 edic)
{ // Externally mark corrupted R1-blocks metadata as writtern, Also removing bad sector / never-written markers if existed
	nvmeibc_block_dp_ec_md_make_r1(md, edic);
	nvmeibc_block_dp_ec_md_fill_p_with_dbits(md, zero_dbits);
	nvmeibc_block_dp_ec_md_clean_reserved(md);
}

static inline void nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_r1(union nvmeibc_block_dp_ec_data_block_md *md, const u32 edic)
{ // Function is used to fix corrupted Raid1-blocks metadata externally
	if (nbdpec_md_was_data_never_written(md)) {
		nbdpec_md_mark_data_never_written_no_dbits(md, NVMEIBC_DATA_MD_MIRROR_IS_PARITY);
	} else {
		nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_r1(md, edic);
	}
}

struct nvmeibc_d_iocmd_comp;
struct nvmeibc_block_command;

// For non QLC volumes with MD
int nvmeibc_check_metadata_read_cmd(struct nvmeibc_block_command *cmd, u64 rlba, const u32 mask, void *md, const u32 md_size, const int slice_size, const int snake_size, const int prev_rv);

// Post-read actions required to verify read data integrity (version, edic etc)
void nvmeibc_check_metadata_actions(struct nvmeibc_d_iocmd_comp *comp);

// Copy metadata from one command to another while preserving correct buffer alignment specific to disk. Assume both non-NULL
void nvmeibc_fill_metadata_from_command(struct nvmeibc_block_command *destination_command, const struct nvmeibc_block_command *source_command);
bool nvmeibc_are_metadatas_identical(const struct nvmeibc_block_command *c0, const struct nvmeibc_block_command *c1);


// In mirror, metadata is enabled iff disk is formatted with metadata, and it is not explicitly disabled
#define nvmeibc_is_mirror_md_enabled(cmd) ((nvmeibc_sgmnt_sw_md_size((cmd)->ds) > 0) && (cmd)->o->nd->dp.enable_edic_check)

/**
 * Allocate metadata with accordance to the configuration
 * @param nlbas  Number of lbas to allocate metadata for
 * @param mdsize The size of the metadat to allocate for each lba
 */
void *nvmeibc_alloc_md(const u64 nlbas, const u32 mdsize);

/**
 * Free metadata with accordance to the configuration
 * @param md pointer to the metadata
 */
void nvmeibc_free_md(void *md);

#endif // NVMEIBC_BLOCK_DP_EC_BLOCK_MD_H
