#ifndef NVMEIB_SHARED_H
#define NVMEIB_SHARED_H

#include <linux/types.h>
#include "nvmeib_linux_code_version.h"
#include "../common_public/nvmeib_uuid_be.h"
#include "compat/kr_incs_time.h"
#include "nvmeib_str.h"
/* Get access to struct nvmeib_buffer */
#include "nvmeib_buffer.h"
#include "compat/kr_incs_bit_ops.h"

#ifdef UM_APP
#define _N_dmesg(trace_level, name, ...) _NI_dmesg(name, ## __VA_ARGS__)
#endif
typedef enum {T_INFO, T_WARN, T_ERROR, T_BUG} nvmeib_trace_level;

#define _NVMEIB_MSB_IN_1_BITS(x)	((x) >= (1 << 0)	? 1											: 0							)
#define _NVMEIB_MSB_IN_2_BITS(x)	((x) >= (1 << 1)	? 1  + (_NVMEIB_MSB_IN_1_BITS((x) >> 1))	: _NVMEIB_MSB_IN_1_BITS(x)	)
#define _NVMEIB_MSB_IN_4_BITS(x)	((x) >= (1 << 2)	? 2  + (_NVMEIB_MSB_IN_2_BITS((x) >> 2))	: _NVMEIB_MSB_IN_2_BITS(x)	)
#define _NVMEIB_MSB_IN_8_BITS(x)	((x) >= (1 << 4)	? 4  + (_NVMEIB_MSB_IN_4_BITS((x) >> 4))	: _NVMEIB_MSB_IN_4_BITS(x)	)
#define _NVMEIB_MSB_IN_16_BITS(x)	((x) >= (1 << 8)	? 8  + (_NVMEIB_MSB_IN_8_BITS((x) >> 8))	: _NVMEIB_MSB_IN_8_BITS(x)	)
// Return the bit no of the MSB. Useful for compile-time constants. Examples: MSB(0)=0, MSB(0xf)=4, MSB(0x10)=5
#define NVMEIB_MSB_IN_32_BITS(x)	((x) >= (1 << 16)	? 16 + (_NVMEIB_MSB_IN_16_BITS((x) >> 16))	: _NVMEIB_MSB_IN_16_BITS(x)	)
#define NVMEIB_MSB_IN_64_BITS(x)	((x) >= (1 << 32)	? 32 + (NVMEIB_MSB_IN_32_BITS((x) >> 32))	: NVMEIB_MSB_IN_32_BITS(x)	)
#define NVMEIB_XHASHTABLE_N_BITS(size)	(NVMEIB_MSB_IN_32_BITS(size) ?  : 1)

/* the maximun number of resources allocated on the admin channel
	   for io requests
	*/
#ifndef LOW_MEM
#define NVMEIB_MAX_DISK_RESOURCES_PER_CLIENT 		(128)
#define NVMEIB_MAX_NORDDA_IO_REQ					(96)
#else
#define NVMEIB_MAX_DISK_RESOURCES_PER_CLIENT 		(1)
#define NVMEIB_MAX_NORDDA_IO_REQ					(4)
#endif
#define NVMEIB_NORDDA_SRQ_MAX_SIZE (2*NVMEIB_MAX_NORDDA_IO_REQ)

#ifndef NVMEIBC_SECTOR_SHIFT
#define NVMEIBC_SECTOR_SHIFT	(12)
#endif

#define LOCK_CHANGE_STRIDE_SHIFT 1	// (When 1 <<) How many blksets are locked in seg before switching to locking on the next seg

/* Only define for TOMA in order to prevent clashes with kernel blkdev.h */
#ifdef TOMA
	#define SECTOR_SIZE (0x1 << NVMEIBC_SECTOR_SHIFT)
	#if (SECTOR_SIZE != 4096)
	#	error OOPS, we do not support such sector size
	#endif
#endif

/* Data-length to Metadata-length */
#define NVMEIB_D2MD_LEN(_dlen_, _bshift_, _md_size_) ((_dlen_ >> _bshift_) * _md_size_)

/* binje - nummber of (4KB) 'Blocks IN Journal Entry'
   values are power of 2s within [1..32] but keeping it flexi */
typedef __be32 __be_binje_t;
typedef u32 binje_t;
#define binje_to_be(binje)		(cpu_to_be32((binje)))
#define binje_from_be(binje)	(be32_to_cpu((binje)))
#define NVMEIB_EC_INVALID_JOURNAL_BINJE ((binje_t)(~0))

#define NVMEIB_EC_INVALID_JOURNAL_RLBA (~(u32)0)

#define NVMEIB_EC_INVALID_JOURNAL_RBLK (~(u32)0)

#define NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES 3   // The number of JRI values which should be reserver starting from 0. values [0,1,2] are reserved.
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define NVMEIB_EC_MAX_JOURNAL_RANGES (NVMEIB_EC_NUM_RESERVED_JOURNAL_RANGES+3)	/* 1-Live client, 1-OtherSimulatedCLient, 1- In multi-instance test (smaller ranges for easier debugging) */
	#define NVMEIB_EC_JOURNAL_RANGES_TRACE 			"@BITMAP8"
	#define NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE 	"@HEX64"
	#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT (6)
	#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT (6)
	#define NVMEIB_EC_JOURNAL_MAXBLKS_PER_RANGE_PRINT "%32pbl"
	#define NVMEIB_EC_JOURNAL_MAXBLKS_PER_RANGE_TRACE "@BITMAP32"
	#define NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE	"@BITMAP64"
#else
	#define NVMEIB_EC_MAX_JOURNAL_RANGES 				1024
	#define NVMEIB_EC_JOURNAL_RANGES_TRACE 				"@BITMAP1024"
	#define NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE 			"@HEX256"
	#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT 		(13U)		/* Use 8192 journal blocks per range (allows 512 entries up to BINJE 16, at the cost of reducing the number of clients or increasing the size of the journal partition) */
	#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT	(9U) 		/* Original value from Version 1.3 of NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT) */
	#define NVMEIB_EC_JOURNAL_MAXBLKS_PER_RANGE_PRINT 	"%8192pbl"
	#define NVMEIB_EC_JOURNAL_MAXBLKS_PER_RANGE_TRACE 	"@BITMAP8192"
	#define NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE	"@BITMAP512"
#endif

#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3	(1U << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT)

/* We cap the max entries at the V1.3 maximum (512) despite the increase in number of jblocks.
 * This solves lots of compatibility and frame-size issues */
#define NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE		(1 << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3_SHIFT) /* 512 */

#define NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE		(1U << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT)

#define NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY 	1U
#define NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY 		1U
#define NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY 		32U

#define NVMEIB_EC_TOTAL_JOURNAL_BLKS		(NVMEIB_EC_MAX_JOURNAL_RANGES << NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT)	// 2^19 blocks
#define NVMEIB_EC_MAX_TOTAL_JOURNAL_ENTRIES	(NVMEIB_EC_TOTAL_JOURNAL_BLKS / NVMEIB_EC_JOURNAL_MIN_BLOCKS_PER_ENTRY)
#define NVMEIB_EC_JOURNAL_SECTOR_SHIFT 		(NVMEIBC_SECTOR_SHIFT)
#define NVMEIB_EC_JOURNAL_SECTOR_MASK 		(~((1 << NVMEIB_EC_JOURNAL_SECTOR_SHIFT) -1))
#define NVMEIB_EC_INVALID_JOURNAL_RANGE		(~(u32)0)
#define NVMEIB_EC_INVALID_JOURNAL_ENTRY		(~(u32)0)
#define NVMEIB_EC_INVALID_JOURNAL_GEN_ID	(~(u64)0)
#define NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID	(0)

#define NVMEIB_EC_INVALID_BLOCKSET_SLBA		(~(u64)0)
#define NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT		(NVMEIBC_SECTOR_SHIFT)

#define NVMEIBS_EC_SERJIO_DB_JRANGE_ENTRY_SIZE		(1 << NVMEIB_EC_SERJIO_DB_JRANGE_ENTRY_SHIFT)
#define NVMEIB_EC_SERJIO_DB_SIZE 			(NVMEIB_EC_MAX_JOURNAL_RANGES*NVMEIBS_EC_SERJIO_DB_JRANGE_ENTRY_SIZE)

#define EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED_CONST	{.ll = {0xFB2D6A17E4084CC9, 0x95A347C7506E54E5}}
#define EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL_CONST	{.ll = {0x1510F276AB164C60, 0xB3C4847E6F1EA937}}
#define EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD_CONST	{.ll = {0x1234567898765432, 0x1234567898765432}}
#define EXCELERO_DATA_PARTITION_TYPE_GUID_DEL_JOURNAL_CONST {.ll = {0x8F6FD59661F44E6B, 0xB4A6809ED84EA2B9}}
#define EXCELERO_METADATA_PARTITION_TYPE_GUID_CONST			{.ll = {0x1C6BD8FA3CA24AA3, 0xA66427C4AB070B9D}}
#define EXCELERO_METADATA_UPGRADE_PARTITION_TYPE_GUID_CONST	{.ll = {0x51BF5B8DBC584C56, 0xA8849B75CC96275B}}
#define EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD_CONST		{.ll = {0x9876543212345678, 0x9876543212345678}}
#define EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID_CONST 	{.ll = {0x7A0F8886426A4B11, 0x90FEFD1441D8CE3B}}
#define EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID_CONST 		{.ll = {0xFC553B0944E54508, 0xAE79D05163FFD494}}
#define EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID_OLD_CONST	{.ll = {0x13579BDF98765432, 0x13579BDF98765432}}
#define EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID_OLD_CONST 	{.ll = {0x1234432156789876, 0x1234432156789876}}

#define EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED		((const union nvmeib_uuid)EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED_CONST)  /*Used to fill partition type of segment partitions in GPT, */
#define EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL	((const union nvmeib_uuid)EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL_CONST)  /*Used to fill partition type of segment partitions in GPT, */
#define EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD		((const union nvmeib_uuid)EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD_CONST)  /*Used to fill partition type of old segment partitions in GPT, */
#define EXCELERO_METADATA_PARTITION_TYPE_GUID			((const union nvmeib_uuid)EXCELERO_METADATA_PARTITION_TYPE_GUID_CONST)
#define EXCELERO_METADATA_UPGRADE_PARTITION_TYPE_GUID	((const union nvmeib_uuid)EXCELERO_METADATA_UPGRADE_PARTITION_TYPE_GUID_CONST)
#define EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD		((const union nvmeib_uuid)EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD_CONST)
#define EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID 		((const union nvmeib_uuid)EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID_CONST)
#define EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID 			((const union nvmeib_uuid)EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID_CONST)
#define EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID 		((const union nvmeib_uuid){.ll = {0x02468ACECA864202, 0x02468ACECA864202}})	// Resides in our internal GPT (in the metadata partition)
#define EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID 	((const union nvmeib_uuid){.ll = {0x8877665544332211, 0x2233445566778899}})	// Resides in our internal GPT (in the metadata partition)
#define EXCELERO_DISK_UUID_1_2_UNKNOWN_AWAIT_MGMT		((const union nvmeib_uuid){.ll = {0x49840866709b6e56, 0x926c2733f0449587}})	// 709b6e56-0866-4984-8795-44f033276c92
#define GPT_UNUSED_ENTRY_TYPE_GUID 						((const union nvmeib_uuid){.ll = {0, 0}})
#define GPT_BROKEN_GUID 								((const union nvmeib_uuid){.ll = {0x6666666666666666, 0x6666666666666666}})
#define MBR_SIGNATURE   0xAA55
#define PMBR_OS_TYPE    (char)0xEE
#define NVMESH_FORMATTED_HDR	"NVMESH_FORMATTED_DISK"

#define EXCELERO_JOURNAL_DATA_PARTITION_ATTRIBUTE_DEPRECATED_MASK (1ULL << 50)

#define DISK_DATA_INIT_BYTE (0x00)
#define DISK_MD_INIT_BYTE (0xff)
#define DISK_MIN_MD_SIZE_BYTE (8)
#define DISK_MAX_MD_SIZE_BYTE (64)
#define DISK_ALLOW_INLINE_MD	(0)
#define NVME_NS_MC_INLINE_MASK (0x1)
#define NVME_NS_MC_SEP_MASK (0x2)

#define GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES (128)

/* sizeof(struct nvmeibt_client_msg) +
sizeof(struct nvmeibt_client_topo_praid) +
sizeof(struct nvmeibt_client_topo_disk_segment) * N_MAX_RAID_SLICE_LEN*/
#define NVMEIB_TOMA_REQ_MAX_LEN_NON_EC (1536)
#define NVMEIB_TOMA_REQ_MAX_LEN	(3248)			/* server <--> client */		// EC-3152: Defined by the largest possible message (toma topology to client), but cant include this file

/******************************************************************************/
#define N_MAX_RAID_SLICE_LEN      (12)	/* Maximum raid length (10+2) Raid6, 16 bits is enough */
#ifndef __bitwise
	#define __bitwise
#endif

//TODO: change to u32 to dupport more than 16 replicas
typedef u16 __bitwise raid_role_t; 		// i'th role in praid: {D0, D1, D2, ..., P, Q}
typedef u16 __bitwise raid_sgmnt_t; 	// i'th seg  in praid: {0, 1, 2, ..., (D+P-1)}
typedef u16 __bitwise roles_bmp_t;		// Bitmaps which are used to represent roles of in praid, first bit is D0, last bit is last parity
typedef u16 __bitwise sgmnts_bmp_t;		// Bitmaps which are used to represent segments groups,   first bit is seg0, last bit is last Seg

#include "nvmeib_shared_ec.h"

static inline bool nvmeibt_is_partition_type_seg_data_partition(const union nvmeib_uuid *partition_type_guid)
{
	return (ARE_UUID_EQ(partition_type_guid, &EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED) ||
			ARE_UUID_EQ(partition_type_guid, &EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL) ||
			ARE_UUID_EQ(partition_type_guid, &EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD));
}

/*****************************************************************/
union nvmeib_lock_id {
	struct {
		u32 idx_in_praid : 4;	//  0 -  3 - Index of seg in pRAID. Supports up to 16 seg raids
#define NVMEIB_REG_LOCK_ID_LOCKID_BITS 	24
#define NVMEIB_REG_LOCK_ID_LOCKID_BITS_MASK 	((1 << NVMEIB_REG_LOCK_ID_LOCKID_BITS) - 1)
		u32 lock_id      : 24;	//  4 - 27 - Per TOMA allocated unique lock id for session
		u32 is_stale     : 1; 	// 28      - Stale bit
		u32 is_read      : 1;	// 29      - Lock was taken for read op (no journal)
		u32 reserved     : 2;	// 30 - 31 - Reserved for future use
	} bits __attribute__((packed));
	struct {
		u32 lock_id_bits : 28;	//  0 - 27 - Total bits composing/relevant or the lock_id
		u32 __remainder  : 4;
	} __attribute__((packed));
	u32 all;
};

static inline int nvmeib_lock_id_to_str(char *buf, size_t len, long arg) {
	const union nvmeib_lock_id *lock_id = (union nvmeib_lock_id *)&arg;
	ssize_t count                       = 0;

	count += scnprintf(buf + count, len - count, "(all=0x%x,idx=%u,lock_id=0x%x", lock_id->all,
	                   lock_id->bits.idx_in_praid, lock_id->bits.lock_id);
	if (lock_id->bits.is_stale)
		count += scnprintf(buf + count, len - count, ",stale");
	if (lock_id->bits.is_read)
		count += scnprintf(buf + count, len - count, ",read");
	count += scnprintf(buf + count, len - count, ")");
	return count;
}

static inline u32 nvmeib_lockid_purify(union nvmeib_lock_id lockid)
{
	return lockid.lock_id_bits;
}

static inline bool nvmeib_lockid_are_purified_eq(union nvmeib_lock_id lockid1, union nvmeib_lock_id lockid2)
{
	return nvmeib_lockid_purify(lockid1) == nvmeib_lockid_purify(lockid2);
}

/******************** Dirty bits definition ***********************************/
#define NVMEIB_DBITS_IN_RAM_MARKER_LEGNTH (12)
union nvmeibc_dbits_entry {			// 12[bits] field Supports raids of up to 15 segments, with 2 parities (Raid6 or 3-replica)
	struct {						// Global mode: Supports 2 disks down, unknown values but no info per slice, only global per blockset
		union {
			struct {					// 4 bits Global mode marker
				u16 mod_marker   : 4;	// Special value: {0,D,E,F} Meaning we are in global mode. 1..C means we are in slice mode
				u16 dead0        : 4;	// First degraded, if exists. F for unknown
			} __attribute__((packed));
			struct {					// Use only if above marker is correct
				u16 is_d0_convict : 1;	// Is d0 dirty convict (must rebuild entire blockset regardless of metadata dbits)
				u16 is_d1_convict : 1;	// Is d1 dirty convict (must rebuild entire blockset regardless of metadata dbits)
				u16 _dont_use1    : 6;	// Access via dead0
			} __attribute__((packed));
		} __attribute__((packed));
		struct {
			u16 dead1		: 4;		//  First degraded, if exists. F for unknown. Support up to raid-6. Note: d0 >= d1 always!
			u16 unused		: 4;		// 12..15 bits. Must not be used!
		} __attribute__((packed));
		/* Note: Here 0xFF0 is special case of both unknowns, 0x000 mean no dbits at all. */
		/* Convict Examples: 0x3FE is seg 2 degraded convict + Unknown second degraded, 0x35F is seg2 and seg4 are both degraded convicts */
		/* Note: Any value of 0x?ij where i < j is reserved for future use */
	} __attribute__((packed)) bsmod;
	struct {							// Slice mode: Supports single degraded but with per slice in blockset info
		u16 dead0		: 4;			// Index of dead seg in praid +1. Legal values {1..C} ==> Supports up to 12 segments (0..11)+1
		u16 fst_slc	    : 5;			// Index of First dirty slice (0..31)
		u16 len_slc	    : 3;			// Continous length of dirty slices. Values [0..7] mean length of [1..8].
		u16 unuseds     : 4;
		/* Note: Here 0x?FF is unknown range, have to scan slice by slice during rebuild, much like in dirty suspect */
		/* Note: Here 0x?{i:5,j:3} such that i+j>=31 is resevered for future use, except for i=0x1F, j=0x7 which is 0x?FF */
	} __attribute__((packed)) slmod;
	struct {							// Used to allow access to all dirty bits as a single field
		u16 bits		: 12;
		u16 unuseds		: 4;
	} __attribute__((packed)) dirty;
	u16 all_bits	: NVMEIB_DBITS_IN_RAM_MARKER_LEGNTH;	// Used to read/write entire struct at once
} __attribute__((packed));
static const union nvmeibc_dbits_entry zero_dbits = {.all_bits = 0};

#define nvmeibc_dbits_entry_is_global_mode(e) \
		((((e)->bsmod.mod_marker) == 0) || ((e)->bsmod.mod_marker > 0xc))

#define UNK_DB	(0xF)				// Nibble which represent unknown dbit
#define DBITS_BUF_ADD(buf, len, i, i_c) ({  \
	size_t __cnt = 0; \
	if (i == UNK_DB) __cnt = scnprintf((buf), (len), "U%c" , i_c); \
	else if (i == 0) __cnt = scnprintf((buf), (len), "N%c" , i_c); \
	else	         __cnt = scnprintf((buf), (len), "%d%c", i-1, i_c); \
	__cnt;  \
})

static inline bool nvmeibc_dbits_has_unknowns(const union nvmeibc_dbits_entry *e)
{
	if (nvmeibc_dbits_entry_is_global_mode(e))
		return ((e->bsmod.dead0 == UNK_DB)||(e->bsmod.dead1 == UNK_DB));
	return false;
}

static inline int nvmeibc_dbits_entry_to_str(char *buf, int len, long arg) {
	const union nvmeibc_dbits_entry *e = (const union nvmeibc_dbits_entry *)&arg;
	ssize_t count                      = 0;
	count += scnprintf(buf + count, len - count, "(");
	if (nvmeibc_dbits_entry_is_global_mode(e)) {
		if (e->all_bits == 0) {
			count += scnprintf(buf + count, len - count, "None");
		} else {
			const char i0_c = (e->bsmod.is_d0_convict ? 'c' : ' ');
			const char i1_c = (e->bsmod.is_d1_convict ? 'c' : ' ');
			count += DBITS_BUF_ADD(buf + count, len - count, e->bsmod.dead0, i0_c);
			count += scnprintf(buf + count, len - count, ",");
			count += DBITS_BUF_ADD(buf + count, len - count, e->bsmod.dead1, i1_c);
		}
	} else {
		count += DBITS_BUF_ADD(buf + count, len - count, e->slmod.dead0, ' ');
		count += scnprintf(buf + count, len - count, ", {%d,%d}", e->slmod.fst_slc, e->slmod.len_slc);
	}
	count += scnprintf(buf + count, len - count, ")");
	return count;
}
#undef DBITS_BUF_ADD

static inline union nvmeibc_dbits_entry nvmeib_dbits_entry_single_unk(void)
{
	union nvmeibc_dbits_entry rv = {.all_bits = 0};
	rv.bsmod.dead0 = 0xF;
	return rv;
}

static inline union nvmeibc_dbits_entry nvmeib_dbits_entry_build_unk(/* seg index or -1 if irrelevant */	int conv0, int conv1)
{
	union nvmeibc_dbits_entry rv = {.all_bits = 0};
	if (conv0 < conv1)	// sort the values. Use: #define swap(x, y)
		{ u32 tmp = conv0; conv0 = conv1; conv1 = tmp; }
	if (conv0 >= 0) {
		rv.bsmod.is_d0_convict = true;
		rv.bsmod.dead0 = conv0+1;
	} else {
		rv.bsmod.dead0 = 0xF;
	}
	if (conv1 >= 0) {
		rv.bsmod.is_d1_convict = true;
		rv.bsmod.dead1 = conv1+1;
	} else {
		rv.bsmod.dead1 = 0xF;
	}
	if (rv.bsmod.mod_marker)
		rv.bsmod.mod_marker += 0xc;	// All convicts are encoded as 0xC
	if (rv.bsmod.dead0 < rv.bsmod.dead1) {
		u16 tmp = rv.bsmod.dead0;
		rv.bsmod.dead0 = rv.bsmod.dead1;
		rv.bsmod.dead1 = tmp;
		tmp = rv.bsmod.is_d0_convict;
		rv.bsmod.is_d0_convict = rv.bsmod.is_d1_convict;
		rv.bsmod.is_d1_convict = tmp;
	}
	return rv;
}

static inline union nvmeibc_dbits_entry nvmeib_dbits_entry_build_for_seg(/* seg index or -1 if irrelevant, R1 values:0,1 */	int seg0)
{
	union nvmeibc_dbits_entry rv = {.all_bits = 0};
	if (seg0 >= 0) {
		rv.slmod.dead0 = seg0+1;
	}
	return rv;
}

static inline union nvmeibc_dbits_entry nvmeib_dbits_entry_build_for_segs(/* seg index or -1 if irrelevant, R1 values:0,1 */	int seg0, int seg1)
{
	union nvmeibc_dbits_entry rv = {.all_bits = 0};
	seg0++; seg1++;
	if (seg0 > seg1) {
		rv.bsmod.dead0 = seg0;
		rv.bsmod.dead1 = seg1;
	} else {
		rv.bsmod.dead0 = seg1;
		rv.bsmod.dead1 = seg0;
	}
	return rv;
}

/*************************** blkset info (32 bit) *****************************/
union nvmeib_blkset_info {
	struct {
#define NVMEIB_BLKSET_INFO_TXID_SHIFT 	0											// Defines describing the below, for future upgrade
#define NVMEIB_BLKSET_INFO_TXID_MASK 	((1 << NVMEIB_EC_JMDC_BITS_TX_ID)-1)		// 0x000fffff
#define NVMEIB_BLKSET_INFO_DIRTY_MASK 	(((u32)(~0))^NVMEIB_BLKSET_INFO_TXID_MASK)	// 0xfff00000
		u32 txid		: NVMEIB_EC_JMDC_BITS_TX_ID;		// 0 - 19 - Transaction ID
		u32 dirty		: NVMEIB_DBITS_IN_RAM_MARKER_LEGNTH;// 20 - 31 - Dirty Bits
	} bits __attribute__((packed));
	u32 all;
};

static inline int nvmeib_blkset_info_to_str(char *buf, int len, long arg) {
	union nvmeib_blkset_info binfo = {.all = arg};
	ssize_t count                   = 0;
	count += scnprintf(buf + count, len - count, "txid=0x%x,", binfo.bits.txid);
	count += nvmeibc_dbits_entry_to_str(buf + count, len - count, binfo.bits.dirty);

	return count;
}

/************************ lock_id + binfo (64 bit) ****************************/
#define NVMEIB_LOCK_BLKSET_ENTRY_SIZE	sizeof(union nvmeib_lock_blkset_entry)

union nvmeib_lock_blkset_entry {
	struct {
		union nvmeib_lock_id 	lock_id;
		union nvmeib_blkset_info blkset_info;
	};
	u64 all;
};
static const union nvmeib_lock_blkset_entry nvmeib_lock_init_value = {.all = 0,};
static const union nvmeib_lock_blkset_entry nvmeib_stale_special_raid1 = {{ .lock_id = { .bits = { .idx_in_praid = 0xf, .lock_id = 0xf321, .is_stale = 1, }}}}; // bits not explicitly set are initialized to zero
static const union nvmeib_lock_blkset_entry nvmeib_stale_bit_mask_ec =   {{ .lock_id = { .bits = {                                         .is_stale = 1, }}}};

union nvmeib_blkset_problem_report {	// A report created by target to client of existing blockset problems, which require fixing
	struct {
		u16 dbits : NVMEIB_DBITS_IN_RAM_MARKER_LEGNTH;		// Dbits which require fixing
		u16 binfo_not_commited : 1;							// blockset info in RAM of different servers not commited
		u16 is_stale           : 1;							// Stale lock is present
		u16 reserved1          : 1;
		u16 in_progress        : 1;							// Internal use of client. Server never sets this bit
	} __attribute__((packed));
	u16 all;
} __attribute__((packed));

static inline int nvmeib_blkset_problem_report_to_str(char *buf, int len, long arg) {
	union nvmeib_blkset_problem_report pr = {.all = arg};
	ssize_t count                   = 0;
	count += scnprintf(buf + count, len - count, "problem=");
	count += nvmeibc_dbits_entry_to_str(buf + count, len - count, pr.dbits);
	if (pr.binfo_not_commited)
		count += scnprintf(buf + count, len - count, ",binfo_not_commited");
	if (pr.is_stale)
		count += scnprintf(buf + count, len - count, ",is_stale");
	if (pr.reserved1)
		count += scnprintf(buf + count, len - count, ",reserved1");
	if (pr.in_progress)
		count += scnprintf(buf + count, len - count, ",in_progress");

	return count;
}

static inline union nvmeib_blkset_problem_report nvmeib_blkset_problem_report_fill_new(const union nvmeib_lock_blkset_entry* entry, char get_stale)
{
	union nvmeib_blkset_problem_report rv = {.all = 0};
	rv.dbits    = (u16)              entry->blkset_info.bits.dirty;
	//rv.is_stale = (u16)(get_stale && entry->lock_id.bits.is_stale);	// 2020.04.23, Daniel: This is a di.bug, a stale lock can momentarly become taken by other client so list of stales means stales+taken
	rv.is_stale = (u16)(get_stale && (entry->lock_id.all != 0));
	return rv;
}

union nvmeib_blkset_sparse_report {		// Array can be packed using this struct. Instead of Arr[i] describes problem of blockset i: Arr[i] stores entry {Problem of blockset j}
	struct {
		u32 ind         : 30;			// Index of the blockset on disk. Enough for up to (2^30*BYTES_IN_LOCKSET) disk size: 128TB
		u32 reserved    :  1;
		u32 in_progress :  1;			// Internal use of client. Server never sets this bit
		union {							// Describes a problem in this blockset (not always all 32 bits are used)
			union nvmeib_lock_id l;		// Currently the only type of problem is the value of stale lock
			u32 val;
		};
	};
	u64 all;
};

static inline int nvmeib_blkset_sparse_report_to_str(char *buf, int len, long arg) {
	const union nvmeib_blkset_sparse_report pr = {.all = arg};
	ssize_t count = scnprintf(buf, len, "problem={dlba_blkset=0x%x, in_prog=%d val=0x%x}", pr.ind, pr.in_progress, pr.val);
	// Daniel: Consider using: count += nvmeib_lock_id_to_str(buf + count, len - count, &pr.l.all);
	return count;
}

struct nvmeib_lock_entry_constants {
	bool w_blkset_info;
	u32 blkset_info_dbits_shift;
	u32 blkset_info_dbits_mask;
	u32 blkset_info_txid_shift;
	u32 blkset_info_txid_mask;
	unsigned long long unlocked_val;
	unsigned long long stale_bit_mask;
};
#define nvmeib_lock_entry_constants_is_non_embedded_dbits(c) \
	(!(c)->w_blkset_info || !(c)->blkset_info_dbits_mask)

/*****************************************************************************/
/* client disconnect msg is split to header and payload.
   Two consecutive reads allow the user to get the entire msg */
struct nvmeibs_toma_client_disconnect_msg_hdr {
	u32 cid;
	u32 payload_len;
}__attribute__((packed));

#define NVMEIB_IB_DEVICE_NAME_MAX 64
#define NVMEIB_GID_STR_MAX (URN_UUID_STR_LENGTH+4)	// 37 bytes is enough, typically use 40 for nice allignemnt
#define LOCAL_DISK_STATUS_STR_LEN	32
#define DISK_NAME_LEN			32
#define NODE_ID_MAX_LEN 128

enum {
	NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE_SPEC = 20,	// like: S3HCNX0K501681 (according to NVME spec)
	NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE = 40,		// like: S3HCNX0K501681.1 (Excelero concatenates ID with name space so the string is longer
	NVMEIB_DISK_MAX_MODEL_STR_SIZE = 40,			// like: SAMSUNG MZWLL800HEHP-00003, note: may include spaces and such
	NVMEIB_HOST_NAME_LEN = 64,						// like: nvme111.excelero.com

	NVMEIB_MAX_KERN_VER_STRLEN = 64,
	NVMEIB_MAX_OFED_VER_STRLEN = 64,
};

struct nvmeibs_toma_disk_change_msg {
	u64 n_blocks;
	u64 n_hw_blocks;
	u64 vendor_id;
	char model_str[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	char dev_name[DISK_NAME_LEN*4];				// EC-3152: *2 is very fishy
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	char status[LOCAL_DISK_STATUS_STR_LEN];
	u32 block_size;
	u32 max_request_size;
	u32 seq;
	u32 nsid;
	u32 metadata;
	char op;
	char native_serial_str[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
}__attribute__((packed));

enum nvmeibs_toma_lock_gid_op {
	NVMEIBS_TOMA_LOCK_GID_OP_GET = 0,
	NVMEIBS_TOMA_LOCK_GID_OP_PUT = 1,
};
struct nvmeibs_toma_disk_segment_lock_gid_req {
	enum nvmeibs_toma_lock_gid_op op;
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	int seg_id;
}__attribute__((packed));

#define MAX_PORTS_FOR_LOCKS_GIDS (8)

struct nvmeibs_toma_disk_segment_lock_gid_rsp {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	int seg_id;
	int num_gids;
	u8 gids[MAX_PORTS_FOR_LOCKS_GIDS][16];
}__attribute__((packed));

struct nvmeibs_toma_client_disconnect_msg_req {
	u32 cid;
}__attribute__((packed));

struct nvmeibs_toma_client_disconnect_force_cmd {
	u32 cid;
}__attribute__((packed));

struct nvmeibs_toma_port_gid_change_msg {
	char ib_dev[NVMEIB_IB_DEVICE_NAME_MAX];
	u8 port;
	char gid_str[NVMEIB_GID_STR_MAX];
}__attribute__((packed));

struct nvmeibs_toma_nic_change_msg {
	char ib_dev[NVMEIB_IB_DEVICE_NAME_MAX];
	bool add;
}__attribute__((packed));

enum nvmeibs_serjio_state {
	/* Boot states */
	SERJIO_INIT = 0,
	SERJIO_RD_GPT,
	SERJIO_RD_DB,
	SERJIO_RD_JRNL,
	SERJIO_INIT_JRNL,
	SERJIO_GPT_INIT,

	/* Running states */
	SERJIO_READY,
	SERJIO_CLN_JRNL,
	SERJIO_GPT_UPDATE,

	/* Halt states */
	SERJIO_DYING,

	/* Error States */
	SERJIO_ERR_GENERAL,		/* General Error */
	SERJIO_ERR_GPT,			/* GPT Error */
	SERJIO_ERR_NO_JRNL,		/* No Journal Partition */
	SERJIO_ERR_NO_DB,		/* No Database Partition */
	SERJIO_ERR_RD_DB,		/* Error reading DB */
	SERJIO_ERR_WR_DB,		/* Error writing DB */
	SERJIO_ERR_RD_JRNL,		/* Error reading Journal */
	SERJIO_ERR_WR_JRNL,		/* Error writing Journal */
	SERJIO_ERR_JMDC_OOM,		/* Out-of-Memory Error allocating JMDC */

	MAX_SERJIO_STATE,
};

static inline const char *nvmeib_shared_serjio_state_to_str(enum nvmeibs_serjio_state srj_state)
{
	switch (srj_state) {
	/* Booting states */
	case SERJIO_INIT: return "Init";
	case SERJIO_RD_GPT: return "Read GPT";
	case SERJIO_RD_DB: return "Read DB";
	case SERJIO_RD_JRNL: return "Read Jrnl";
	case SERJIO_INIT_JRNL: return "Init Jrnl";
	case SERJIO_GPT_INIT: return "GPT Init";

	/* Running states */
	case SERJIO_READY: return "Ready";
	case SERJIO_CLN_JRNL: return "Clear Jrnl";
	case SERJIO_GPT_UPDATE: return "GPT Update";

	/* Error states */
	case SERJIO_ERR_GENERAL: 	return "ERROR - General";
	case SERJIO_ERR_GPT: 		return "ERROR - GPT Error";
	case SERJIO_ERR_NO_JRNL: 	return "ERROR - No Journal";
	case SERJIO_ERR_NO_DB: 		return "ERROR - No DB";
	case SERJIO_ERR_RD_DB: 		return "ERROR - Read DB";
	case SERJIO_ERR_WR_DB: 		return "ERROR - Write DB";
	case SERJIO_ERR_RD_JRNL:	return "ERROR - Read Journal";
	case SERJIO_ERR_WR_JRNL: 	return "ERROR - Write Journal";
	case SERJIO_ERR_JMDC_OOM:	return "ERROR - OOM Allocating JMDC";

	/* Halting states */
	case SERJIO_DYING: return "Dying";

	default: return "Unknown State!";
	}
}

enum nvmeibs_serjio_status {
	NVMEIBS_SERJIO_STATUS_NOT_FOUND		= 0,	/* SERJIO was not found */
	NVMEIBS_SERJIO_STATUS_READY			= 1,	/* SERJIO is ready for use */
	NVMEIBS_SERJIO_STATUS_NOT_READY		= 2,	/* Not ready - still reading Journal/DB */
	NVMEIBS_SERJIO_STATUS_ERROR			= 3,	/* Error occurred */
	NVMEIBS_SERJIO_STATUS_NO_MD			= 4,	/* Disk is not formatted with metadata */
	NVMEIBS_SERJIO_STATUS_NO_GPT		= 5,	/* Disk is missing GPT */
	NVMEIBS_SERJIO_STATUS_NO_JOURNAL	= 6,	/* Journal partition does not exist */
	NVMEIBS_SERJIO_STATUS_NO_DB			= 7,	/* SERJIO Database partition does not exist */
	NVMEIBS_SERJIO_STATUS_NOT_SUPP		= 8,	/* Not supported */
	NVMEIBS_SERJIO_STATUS_MAX			= 9,
} __attribute__((packed));

static inline const char *nvmeib_shared_serjio_status_to_str(enum nvmeibs_serjio_status serjio_status)
{
	switch(serjio_status) {
	case NVMEIBS_SERJIO_STATUS_NOT_FOUND:
		return "NOT_FOUND";
	case NVMEIBS_SERJIO_STATUS_READY:
		return "READY";
	case NVMEIBS_SERJIO_STATUS_NOT_READY:
		return "NOT_READY";
	case NVMEIBS_SERJIO_STATUS_ERROR:
		return "ERROR";
	case NVMEIBS_SERJIO_STATUS_NO_MD:
		return "NO_MD";
	case NVMEIBS_SERJIO_STATUS_NO_GPT:
		return "NO_GPT";
	case NVMEIBS_SERJIO_STATUS_NO_JOURNAL:
		return "NO_JOURNAL";
	case NVMEIBS_SERJIO_STATUS_NO_DB:
		return "NO_DB";
	case NVMEIBS_SERJIO_STATUS_NOT_SUPP:
		return "NOT_SUPPORTED";
	default:
		break;
	}
	return "INVALID";
}

enum nvmeibs_serjio_jrange_status {
	JRANGE_UNKNOWN 			= 0x00,
	JRANGE_FREE 			= 0x01,
	JRANGE_RESERVED 		= 0x02,
	JRANGE_ALLOCATED 		= 0x03,
	JRANGE_ERROR			= 0x04,
	/* =============================================*/
	/* Version 1.3 values below			*/
	/* =============================================*/
	/* JRANGE_INVALID - Invalid because there are not enough journal blocks to accomodate this range
	 * at the current value of nvmeibs_jrange_num_blocks */
	JRANGE_INVALID			= 0x05,
	/* Not written to DB */
	JRANGE_DB_ZERO			= 0x10,
	JRANGE_DB_ERR			= 0x11,
	JRANGE_RSVD_DB_ERR		= 0x12,
	/* If JRIs 0,1,2 are allocated on disk due to upgrade, they are moved to invalid list and set to this state.
	 * They are still stored as JRANGE_FREE on disk.
	 */
	JRANGE_QUARANTINED		= 0x13,
};

#define JRANGE_STATUS_DB_MIN 		JRANGE_FREE
#define JRANGE_STATUS_DB_V12_MAX	JRANGE_ERROR
#define JRANGE_STATUS_DB_V13_MAX	JRANGE_INVALID
#define JRANGE_STATUS_DB_MAX		JRANGE_INVALID

#define JRANGE_STATUS_DB_VALID_MIN	JRANGE_FREE
#define JRANGE_STATUS_DB_VALID_MAX	JRANGE_ALLOCATED

static inline const char *nvmeib_shared_serjio_jrange_status_to_str(enum nvmeibs_serjio_jrange_status status)
{
	switch (status) {
	case JRANGE_FREE:
		return "FREE";
	case JRANGE_RESERVED:
		return "RESERVED";
	case JRANGE_ALLOCATED:
		return "ALLOCATED";
	case JRANGE_ERROR:
		return "ERROR";
	case JRANGE_INVALID:
		return "INVALID";
	case JRANGE_DB_ZERO:
		return "DB ZERO";
	case JRANGE_DB_ERR:
		return "DB ERROR";
	case JRANGE_RSVD_DB_ERR:
		return "RESERVED DB ERROR";
	case JRANGE_QUARANTINED:
		return "QUARANTINED";
	default:
		return "UNKNOWN";
	}
}

enum nvmeibs_serjio_jentry_state {
	JENTRY_UNKNOWN 		= 0x00,
	JENTRY_SYNCED		= 0x01,
	JENTRY_FREE 		= 0x02,
	JENTRY_ABND 		= 0x03,
	JENTRY_TAKEN 		= 0x04,
	JENTRY_WAIT_RET		= 0x05,
	JENTRY_IO_ERR		= 0x06,
	JENTRY_INVALID		= 0x07,
	MAX_JENTRY_STATE	= 0x08,
};

static inline const char *nvmeib_shared_serjio_jentry_state_to_str(enum nvmeibs_serjio_jentry_state jentry_state)
{
	switch (jentry_state) {
	case JENTRY_UNKNOWN:
		return "UNKNOWN";
	case JENTRY_SYNCED:
		return "SYNCED";
	case JENTRY_FREE:
		return "FREE";
	case JENTRY_TAKEN:
		return "TAKEN";
	case JENTRY_ABND:
		return "ABANDONED";
	case JENTRY_WAIT_RET:
		return "WAIT_RETURN";
	case JENTRY_IO_ERR:
		return "IO_ERROR";
	case JENTRY_INVALID:
		return "INVALID";
	default:
		break;
	}
	return "ERROR";
}

enum nvmeibs_serjio_jentry_state_chng_reason {
	JENTRY_STATE_CHNG_REASON_UNKNOWN = 0,		/* Unknown reason - Should not be used */
	JENTRY_STATE_CHNG_REASON_EXTERNAL,		/* Set externally via /proc interface - Infrastructure */
	JENTRY_STATE_CHNG_REASON_ZERO_OK,		/* Entry zeroed successfully */
	JENTRY_STATE_CHNG_REASON_ZERO_FAIL,		/* Entry zero failed */
	JENTRY_STATE_CHNG_REASON_SYNC_DIRTY,		/* Synced and was dirty (ie contains IO JMDC) */
	JENTRY_STATE_CHNG_REASON_SYNC_FREE,		/* Synced and was free */
	JENTRY_STATE_CHNG_REASON_SYNC_FAIL,		/* Sync Failed */
	JENTRY_STATE_CHNG_REASON_READ_DIRTY,		/* Read and was dirty (ie contains IO JMDC) */
	JENTRY_STATE_CHNG_REASON_READ_FREE,		/* Read and was free */
	JENTRY_STATE_CHNG_REASON_READ_FAIL,		/* Read Failed */
	JENTRY_STATE_CHNG_REASON_ALLOC_FREE_RNG,	/* Free range was allocated to Client */
	JENTRY_STATE_CHNG_REASON_ALLOC_SERJIO_FREE,	/* Entry was free according to SERJIO on allocation to Client */
	JENTRY_STATE_CHNG_REASON_ALLOC_JAM_FREE,	/* Entry was free according to JAM Cache on allocation to Client */
	JENTRY_STATE_CHNG_REASON_ALLOC_SYNC_FREE,	/* Entry was free after synced on allocation to Client */
	JENTRY_STATE_CHNG_REASON_RET_FREE,		/* Entry was free after range was returned */
	JENTRY_STATE_CHNG_REASON_RET_DIRTY,		/* Entry was dirty after range was returned */
	JENTRY_STATE_CHNG_REASON_RET_INVALID,		/* Entry was invalid after range was returned */
	JENTRY_STATE_CHNG_REASON_CLEAN_SEG,		/* Entry was updated by Clean Segment */
	JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_FREE,	/* Entry was free after Clean Segment Read */
	JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_DIRTY,	/* Entry was dirty after Clean Segment Read */
	JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_IN_SEG,	/* Entry in Segment after Clean Segment Read */
	JENTRY_STATE_CHNG_REASON_FREE_ENTS,		/* Entry was updated by Free Journal Entries */
	JENTRY_STATE_CHNG_REASON_ABND_LOSER,		/* Entry was abandoned by LOSER message */
};

static inline const char *nvmeib_shared_serjio_jentry_state_chng_reason_to_str(enum nvmeibs_serjio_jentry_state_chng_reason reason)
{
	switch (reason) {
	case JENTRY_STATE_CHNG_REASON_UNKNOWN:
		return "Unknown";
	case JENTRY_STATE_CHNG_REASON_EXTERNAL:
		return "External Proc";
	case JENTRY_STATE_CHNG_REASON_ZERO_OK:
		return "Zeroing OK";
	case JENTRY_STATE_CHNG_REASON_ZERO_FAIL:
		return "Zeroing Failed";
	case JENTRY_STATE_CHNG_REASON_SYNC_DIRTY:
		return "Dirty after Sync";
	case JENTRY_STATE_CHNG_REASON_SYNC_FREE:
		return "Free after Sync";
	case JENTRY_STATE_CHNG_REASON_SYNC_FAIL:
		return "Sync Failed";
	case JENTRY_STATE_CHNG_REASON_READ_DIRTY:
		return "Dirty after Read";
	case JENTRY_STATE_CHNG_REASON_READ_FREE:
		return "Free after Read";
	case JENTRY_STATE_CHNG_REASON_READ_FAIL:
		return "Read Failed";
	case JENTRY_STATE_CHNG_REASON_ALLOC_FREE_RNG:
		return "Free Range";
	case JENTRY_STATE_CHNG_REASON_ALLOC_SERJIO_FREE:
		return "SERJIO Free on Allocate";
	case JENTRY_STATE_CHNG_REASON_ALLOC_JAM_FREE:
		return "JAM Free on Allocate";
	case JENTRY_STATE_CHNG_REASON_ALLOC_SYNC_FREE:
		return "Free on Allocate Sync";
	case JENTRY_STATE_CHNG_REASON_RET_FREE:
		return "Free on Return";
	case JENTRY_STATE_CHNG_REASON_RET_DIRTY:
		return "Dirty on Return";
	case JENTRY_STATE_CHNG_REASON_CLEAN_SEG:
		return "Segment Clean";
	case JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_FREE:
		return "Free on Segment Clean Read";
	case JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_DIRTY:
		return "Dirty on Segment Clean Read";
	case JENTRY_STATE_CHNG_REASON_CLEAN_SEG_READ_IN_SEG:
		return "In Segment on Segment Clean Read";
	case JENTRY_STATE_CHNG_REASON_FREE_ENTS:
		return "Free Journal Entries";
	case JENTRY_STATE_CHNG_REASON_ABND_LOSER:
		return "Abandoned by LOSER msg";
	default:
		break;
	}
	return "Invalid";
}

enum nvmeib_rdma_expect_last_wqe {
	NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN = -1,
	NVMEIB_RDMA_DONT_EXPECT_LAST_WQE = 0,
	NVMEIB_RDMA_EXPECT_LAST_WQE = 1,
};

static inline const char *nvmeib_rdma_expect_last_wqe_to_str(enum nvmeib_rdma_expect_last_wqe expect_last_wqe)
{
	switch (expect_last_wqe) {
		case NVMEIB_RDMA_EXPECT_LAST_WQE_UNKNOWN:
			return "LAST_WQE_UNKNOWN";
		case NVMEIB_RDMA_DONT_EXPECT_LAST_WQE:
			return "DONT_EXPECT_LAST_WQE";
		case NVMEIB_RDMA_EXPECT_LAST_WQE:
			return "EXPECT_LAST_WQE";
		default:
			return "INVALID!";
	}
}

enum find_path_cep_state {
	FIND_PATH_EPSTATE_IDLE = 0,
	FIND_PATH_EPSTATE_SOCKET_ERROR,
	FIND_PATH_EPSTATE_CONNECTING,
	FIND_PATH_EPSTATE_CONNECT_ERROR,
	FIND_PATH_EPSTATE_CONNECTED,
	FIND_PATH_EPSTATE_GOT_RESPONSE,
	FIND_PATH_EPSTATE_RELEASING,
	FIND_PATH_EPSTATE_DONE,
	FIND_PATH_EPSTATE_LISTENING
};

static inline const char *find_path_cep_state_to_str(enum find_path_cep_state state)
{
	switch (state) {
	case FIND_PATH_EPSTATE_IDLE: return "IDLE";
	case FIND_PATH_EPSTATE_SOCKET_ERROR: return "SOCKET_ERROR";
	case FIND_PATH_EPSTATE_CONNECTING: return "CONNECTING";
	case FIND_PATH_EPSTATE_CONNECT_ERROR: return "CONNECT_ERROR";
	case FIND_PATH_EPSTATE_CONNECTED: return "CONNECTED";
	case FIND_PATH_EPSTATE_GOT_RESPONSE: return "GOT_RESPONSE";
	case FIND_PATH_EPSTATE_RELEASING: return "RELEASING";
	case FIND_PATH_EPSTATE_DONE: return "DONE";
	case FIND_PATH_EPSTATE_LISTENING: return "LISTENING";
	default: return "UNKNOWN";
	}
}

/* [Gregory]
 * protocol numbers are synced with management_cm schemes &
 * the management module
 */
enum nvmeib_rdma_transport {
	rtr_unknown	= -1,
	rtr_ib		= 0,
	rtr_roce	= 1,
	rtr_tcp		= 2,
	rtr_multi	= 3
};

static inline const char * rdma_trasport_to_string(enum nvmeib_rdma_transport t) {
	switch (t)
	{
		case(rtr_unknown):	return "UNKNOWN";
		case(rtr_ib):		return "INFINIBAND";
		case(rtr_roce):		return "ROCE";
		case(rtr_tcp):		return "TCP";
		case(rtr_multi):	return "MULTI";
		default:			return "UNKNOWN";
	}
}

struct nvmeibs_toma_serjio_state_change_msg {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u16 vendor_id;
	enum nvmeibs_serjio_status serjio_status;
};

struct nvmeibs_toma_serjio_range_cleaned_msg {
	char seg_id[URN_UUID_STR_LENGTH + 1];
};

#define TOMA_THREAD_NAME                "nvmeibt_toma"
#define TOMA_STATUS_PROC_DIR 			"toma_status"
#define TOMA_STATUS_PROC_PATH 			"/proc/nvmeibs/" TOMA_STATUS_PROC_DIR

#define NVMEIBS_CLIENT_NAME_SIZE (NVMEIB_HOST_NAME_LEN + \
								  NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 1 + 7)

/* Journal Shared Structs */
/* journal allocation descriptor per {client, disk} pair - usage:
 * (1) c_disk->jour
 * (2) c_disk->jrc.jour, i.e journal-cache from jam */
struct nvmeibc_disk_client_journal  {
	/* journal-range id */
	u32 rng_id;
	/* first (start) journal-lba in journal-range */
	u64 rng_slba;
	/* number of journal-lbas in journal-range
	 *  which is also number of entries in jmd-tbl */
	u32 rng_nlba;
	/* Generation ID used for recovery */
	u64 rng_gen_id;
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	/* number of 'blocks in jentry' of this jrange */
	binje_t rng_binje;
	/* number of entries in the range */
	u32 n_ents;
	/* number of journal blocks in the range */
	u32 rng_nblk;
	/* max number of jblocks per range (Used by Recovery) */
	u32 max_rng_blk;
	/* total number of journal ranges (Used by Recovery) */
	u32 tot_n_rng;
} __attribute__ ((packed));

struct nvmeib_disk_client_journal_extended{
	uuid_be client_uuid;
	u32 rng_id;
	u64 rng_slba;
	u32 rng_nlba;
	u64 rng_gen_id;
	binje_t binje;
	u32 n_ents;
	u32 rng_blk;

	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	DECLARE_BITMAP(dirty_ents_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	DECLARE_BITMAP(abnd_ents_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
};

/* disk's journal cache from jam:
 * o initialized on nvmeibc_disk_create() via nvmeibc_jam_disk-cache_init()
 * o updated     on disconnect_disk()     via nvmeibc_jam_disk-del() -> jam_disk-cache_fill() (if (disk->jam-disk) is valid) */

/* Metadata for each entry included in JMDC get */
static const u8 nvmeib_jrnl_ent_gen_id_invalid = 0;
static const u8 nvmeib_jrnl_ent_gen_id_min = 1;
static const u8 nvmeib_jrnl_ent_gen_id_max = 255;
static const u64 nvmeib_jrnl_rng_gen_id_min = 1;

struct nvmeib_jrnl_ent_md {
	u8 ent_gen_id; /* Generation ID per entry */
} __attribute__((packed));

struct nvmeibc_jam_range_cache {
	struct nvmeibc_disk_client_journal jour;
	DECLARE_BITMAP(free_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	struct nvmeib_jrnl_ent_md ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	int n_jents;
};

enum nvmeib_gen_cmd_op {
	NVMEIB_GEN_OP_UNUSED = 0,
	NVMEIB_GEN_OP_GET_UUID_JOUR		= 1, /* [clnt: hot recov  ] Req srv to RDMA Write uuid's JMDC and send rsp jour's description */
	NVMEIB_GEN_OP_BLKSET_RECOVERED	= 2, /* [clnt: hot recov  ] Req srv to free jmdc entry of other client, Rsp is sent for debug */
	NVMEIB_GEN_OP_GET_EC_DB 		= 3, /* [clnt: hot recov  ] Req srv to RDMA Write the EC dirty bits */
	NVMEIB_GEN_OP_FREE_JRNL_ENTS 	= 4,
	NVMEIB_GEN_OP_LOCK 				= 5,
	NVMEIB_GEN_OP_GET_JMDC			= 6, /* [clnt: cold recov ] Req serjio for the entire JMDC */
	NVMEIB_GEN_OP_JENTRY_ERASE		= 7, /* [clnt: Jam        ] Req srv to erase journal entry's Data,MD and JMDC, rsp with status */

	MAX_NVMEIB_GEN_OP
};

static inline const char *nvmeib_gen_op_str(enum nvmeib_gen_cmd_op gen_op)
{
	switch (gen_op) {
	case NVMEIB_GEN_OP_GET_UUID_JOUR: 		return "GET_UUID_JOUR";
	case NVMEIB_GEN_OP_BLKSET_RECOVERED:		return "BLKSET_RECOVERED";
	case NVMEIB_GEN_OP_GET_EC_DB: 			return "GET_EC_DB";
	case NVMEIB_GEN_OP_FREE_JRNL_ENTS: 		return "FREE_JRNL_ENTS";
	case NVMEIB_GEN_OP_LOCK: 			return "LOCK";
	case NVMEIB_GEN_OP_GET_JMDC: 			return "GET_JMDC";
	case NVMEIB_GEN_OP_JENTRY_ERASE: 		return "JENTRY_ERASE";
	default: 					return "UNKNOWN GEN OP!";
	}
}

enum __attribute__ ((__packed__)) nvmeib_lock_req_op {
	NVMEIB_ATOMIC_CMP_AND_SWP = 0x1,
	NVMEIB_ATOMIC_FETCH_AND_ADD = 0x2,
	NVMEIB_MASKED_ATOMIC_CMP_AND_SWP = 0x3,
	NVMEIB_MASKED_ATOMIC_FETCH_AND_ADD = 0x4,
	NVMEIB_LOCK_RDMA_READ = 0x05,
	NVMEIB_LOCK_RDMA_WRITE = 0x06,

	NVMEIB_LOCK_LAST = 0xff // keep this LAST
};

struct nvmeib_jmdc_read_jrnl_data {
	/* journal-lba's log2 size (in bytes) */
	u16 lba_shift;
	/* Number of entries in range (when N = 1)*/
	u16 num_ents_rng;
	/* Number of dirty ranges */
	u16 num_dirty_rng;
	/* Number of ranges */
	u16 num_rng;
	/* Start Offset of Journal (in LBA), in units of clnt volume blocks (software lba) */
	u64 jrnl_start_lba;
	/* Length of Journal (in LBA), in units of clnt volume blocks (software lba)  */
	u64 jrnl_len_lba;
	/* Number of entries */
	u32 num_ents;
	/* Number of entry blocks */
	u32 num_ent_blocks;
	/* Range header version - Should match the version negotiated on admin connect */
	u16 rng_hdr_version;
	/* Range header stride */
	u16 rng_hdr_stride;
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
}  __attribute__((packed));

/* New Data structures for Cold Recovery Data */
struct nvmeib_get_jmdc_rng_data {
	/* Client UUID */
	uuid_be client_uuid;
	/* journal-range index */
	unsigned rng_idx;
	/* Size of journal-range (in LBA), in units of clnt volume blocks (software lba) */
	unsigned rng_size_lba;
	/* Start of Journal Range (in LBA), in units of clnt volume blocks (software lba) */
	u64 rng_start_lba;
	/* Range Generation ID (used to keep track of recoveries) */
	u64 rng_gen_id;
	/* If set, only the dirty entries were sent (JMDC and MD), otherwise all entries were sent */
	bool only_dirty_ents;
	/* First Entry Offset in (JMDC (only for N = 1) and MD) array */
	unsigned rng_ent_offset;
	/* Dirty Entries Bitmap */
	DECLARE_BITMAP(dirty_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	/* Abnd Entries Bitmap */
	DECLARE_BITMAP(abnd_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	/* First Entry Block Offset in JMDC array */
	unsigned rng_ent_block_offset;
	/* Blocks in journal entry */
	binje_t binje;
	/* Number of journal entries in range */
	unsigned num_ents;
	/* Number of dirty entries in range */
	unsigned num_dirty_ents;
};

struct nvmeib_free_ents_data {
	/* Range Generation ID */
	u64 rng_gen_id;
	/* journal-range index */
	u16 rng_idx;
	/* Entry Index */
	u16 ent_idx;
	/* Range N */
	binje_t rng_binje;
	/* Entry MD (Generation ID) */
	struct nvmeib_jrnl_ent_md ent_md;
};

struct nvmeib_gen_cmd_lock_rsp {
	int comp_code;
	u64 cmp_swap_val;
	u32 read_len;
	const void *read_data;
};


struct nvmeib_remote_access_info {
	u64 raddr;
	u32 len;
	u32 rkey;
};

struct nvmeib_gen_cmd_data {
	struct nvmeib_buffer local;
	struct nvmeib_remote_access_info remote;
};

enum __attribute__ ((__packed__)) nvmeib_lock_table {
	NVMEIB_LOCK_TABLE_DEPRECATED = 0x00,	// NVMESH-4574 active-locks-deprecation
	NVMEIB_LOCK_TABLE_OWNER = 0x01
};

enum nvmeib_recov_src {
	NVMEIB_RECOV_SRC_BOOT 		= 0x00,	/* SERJIO found invalid entries on boot */
	NVMEIB_RECOV_SRC_COLD 		= 0x01,	/* Cold Recovery */
	NVMEIB_RECOV_SRC_HTR 		= 0x02,	/* HTR */
	NVMEIB_RECOV_SRC_JGC 		= 0x03,	/* Journal Garbage Collection */
	NVMEIB_RECOV_SRC_SEG_D2W 	= 0x04,	/* Segment the entry was pointing at moved from Dead to Write */
	NVMEIB_RECOV_SRC_SEG_DEL	= 0x05, /* Segment the entry was pointing at is being deleted */
	NVMEIB_RECOV_SRC_PROC_FILE	= 0x06, /* Entry was modified through the proc file for testing */
	MAX_NVMEIB_RECOV_SRC,
};

static inline const char *nvmeib_recov_src_str(enum nvmeib_recov_src src)
{
	switch (src) {
		case NVMEIB_RECOV_SRC_BOOT:
			return "BOOT";
		case NVMEIB_RECOV_SRC_COLD:
			return "COLD";
		case NVMEIB_RECOV_SRC_HTR:
			return "HTR";
		case NVMEIB_RECOV_SRC_JGC:
			return "JGC";
		case NVMEIB_RECOV_SRC_SEG_D2W:
			return "SEG_D2W";
		case NVMEIB_RECOV_SRC_SEG_DEL:
			return "SEG_DEL";
		case NVMEIB_RECOV_SRC_PROC_FILE:
			return "PROC";
		default:
			return "UNKNOWN!";
	}
}

struct nvmeib_gen_cmd_lock_param {
	u64 seg_id;
	u64	offset;
	struct {
		u64 compare_add;
		u64 swap;
		u64 compare_add_mask;
		u64 swap_mask;
	} atomic;
	struct {
		enum nvmeib_lock_table table_type;
		u32 len;
		void *data;
	} rdma;
	enum nvmeib_lock_req_op op;
};

enum nvmeib_gen_cmd_jam_ent_erase_reason {
	JAM_ENT_ERASE_REASON_UNKNOWN = 0,
	JAM_ENT_ERASE_REASON_FAILED_WRITE, /* JAM needs to erase entry because of a failed write */
	JAM_ENT_ERASE_REASON_SERJIO_REQUEST, /* SERJIO requested JAM to erase the entry (Delete Segment Flow) */
};

static inline const char *get_jam_ent_erase_reason(enum nvmeib_gen_cmd_jam_ent_erase_reason reason)
{
	switch(reason) {
		case JAM_ENT_ERASE_REASON_FAILED_WRITE:
			return "Failed Write";
		case JAM_ENT_ERASE_REASON_SERJIO_REQUEST:
			return "SERJIO Request";
		default:
			return "UNKNOWN!";
	}
}

struct nvmeib_gen_cmd_jam_ent_erase {
	u16 ent_idx;
	struct nvmeib_jrnl_ent_md ent_md;
	u64 ent_swlba;
};

union nvmeib_gen_cmd_rsp;
typedef void nvmeib_local_gen_cmd_cb_t(union nvmeib_gen_cmd_rsp *, int rv);

struct nvmeib_gen_cmd_param
{
	nvmeib_local_gen_cmd_cb_t *async_cb;
	union {
		struct nvmeib_gen_cmd_lock_param lock_param;
		struct {				// NVMEIB_GEN_OP_GET_UUID_JOUR
			uuid_be client_uuid;
			char sgmnt_uuid[NVMEIB_GID_STR_MAX];
			struct nvmeib_gen_cmd_data jmdc_dest;
			struct nvmeib_gen_cmd_data ent_md_dest;
			binje_t binje;
		} uj;
		struct {				// NVMEIB_SEND_BLKSET_RECOVERED
			uuid_be uuid;
			char ds_uuid[NVMEIB_GID_STR_MAX];
			u64 lock_ent;
			u64 blkset_num;		// Blockset number in praid, used by Toma
			u64 blkset_slba;	// Disk lba of first block in blockset (used by serjio)
			u32 rng_id;
			u32 ent_id;
			u8 pass2toma;
			u64 gen_id;
		} br;
		struct {				// NVMEIB_RDMA_GET_EC_DB
			u64 lba;
			u32 n_lba;
			char get_dbits;		// True -> bitmap of dbits.
			char get_stales;	// True -> bitmap of stale locks. Can combine both
			char get_full_val;	// If true, Get full value otherwise (!!val)
			char reserved;		// Removed in V-1.3.1
			u32 rsp_nlbas;
			struct nvmeib_gen_cmd_data db_dest;
		} db;
		struct {				/* NVMEIB_RDMA_FREE_JRNL_ENTS */
			char serjio_boot_id[NVMEIB_GID_STR_MAX];
			char seg_uuid[NVMEIB_GID_STR_MAX];
			enum nvmeib_recov_src src;
			unsigned num_ents;
			/* Used for HTR */
			u8 pass2toma;
			u64 blkset_num;
			u64 blkset_slba;
			u64 lock_ent;
			struct nvmeib_free_ents_data *ents;
		} free_ents;
		struct {
			char seg_uuid[NVMEIB_GID_STR_MAX];
			u16 start_rng;
			u16 num_rng;
			struct nvmeib_get_jmdc_rng_data *rng_data;
			struct nvmeib_jrnl_ent_md *ent_md;
			size_t rng_data_len;
			size_t ent_md_len;
			bool get_len_only;
			bool get_dirty_only;
			bool get_by_client_uuid;
			enum nvmeib_recov_src src;
			uuid_be client_uuid;
			struct nvmeib_gen_cmd_data jmdc_sink;
		} jmdc_get;
		struct {
			u64 rng_gen_id;
			u32 rng_idx;
			binje_t rng_binje;
			struct nvmeib_gen_cmd_jam_ent_erase ent_erase;
		} je;
	};
};

union nvmeib_gen_cmd_rsp {
	struct nvmeib_gen_cmd_rs_uj {
		struct nvmeib_disk_client_journal_extended jour;
		u32 jmdc_len;
		u32 ent_md_len;
	} uj;
	struct nvmeib_gen_cmd_lock_rsp lock_rsp;
	struct {
		uuid_be uuid;
		u32 rng_id;
		u32 ent_id;
		int status;
	} br;
	struct {
		u32 nlba;
	} db;
	struct {
		struct nvmeib_jmdc_read_jrnl_data jrnl_data;
		size_t hdr_len;
		size_t ents_md_len;
		size_t ents_len;
	} jmdc_get;
};

/* Describes both how the resources are managed and the messages which sends
   them to server (for simplicity reuse the same struct) */
struct nvmeibs_lost_srv_resource_payload {
	DECLARE_BITMAP(bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);	// Bitmap of abandoned journals in clients JRI
	u8 gen_ids[         NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];   // Generation ID of abandoned journal entries
	union {
		struct {				// Fields for msg sending
			u8 code;
			u8 reserverd[3];
		} msg;
		#ifdef DEBUG_LOSER_CONDITIONS
			u32 dummy;		// Ficticious resource used for debug: 1. Verify all resources are reported, 2. Verify report sent to correct segment
		#endif
	};
}__attribute__((packed));

#define NVMEIBS_LOST_SRV_RESOURCE_PAYLOAD_SIZE (sizeof(struct nvmeibs_lost_srv_resource_payload))

#define NVMEIBS_NICS_CSV_HEADER "device,hw_gid,port,pkey,transport,state,mtu,max_mtu,gid_index,roce_v2,roce_ipv6,used,ndev_name,sw_gid"
#define NVMEIBS_NICS_CSV_HEADER_EOL NVMEIBS_NICS_CSV_HEADER "\n"
#define NVMEIBS_DISKS_CSV_HEADER 	"id,blocks,hw_blocks,block_size,max_request_size,seq,nsid,dev_name,metadata,status,vendor,model,native_serial"
#define NVMEIBS_DISKS_CSV_HEADER_EOL 	NVMEIBS_DISKS_CSV_HEADER "\n"
#define NVMEIBS_NICS_CSV_TRANSPORT_INFINIBAND 'I'
#define NVMEIBS_NICS_CSV_TRANSPORT_ROCE 'R'
#define NVMEIBS_NICS_CSV_TRANSPORT_TCP 'T'
#define NVMEIBS_NICS_CSV_TRANSPORT_MULTI 'M'
#define NVMEIBS_NICS_CSV_TRANSPORT_UNDEF 'X'
#define NVMEIBS_SRV_NICS_CSV_MAX_LINE_LEN 255

#define NVMEIBS_SRV_SW_GIDS_CSV_HEADER "gid_index,gid,gid_type,net_type,gid_ip,net_dev,is_vlan,vlan_id,preferred"
#define NVMEIBS_SRV_SW_GIDS_CSV_HEADER_EOL NVMEIBS_SRV_GIDS_CSV_HEADER "\n"

#define NVMEIBS_SERJIOS_CSV_HEADER 				"disk,status,num_rng,free_rng,jrnl_lba,jrnl_nlbas,db_lba,db_nlbas,max_ents_rng,n_blk_rng"
#define NVMEIBS_SERJIOS_CSV_HEADER_EOL			NVMEIBS_SERJIOS_CSV_HEADER "\n"

#define NVMEIBS_DISKS_CSV_STATUS_LEN		32

/* Endianess extra 64b conversion.
 * Mellanix IB defines the vanilla htonll(), ntohll() as inlines without a macro to allow conditional code, hence using a unique name to prevent dependency */
#define nvmeib_htonl(x)	({ _Static_assert(sizeof(x) == 4, "size != 4"); htonl(x); })
#define nvmeib_ntohl(x)	({ _Static_assert(sizeof(x) == 4, "size != 4"); ntohl(x); })
#define nvmeib_htons(x)	({ _Static_assert(sizeof(x) == 2, "size != 2"); htons(x); })
#define nvmeib_ntohs(x)	({ _Static_assert(sizeof(x) == 2, "size != 2"); ntohs(x); })
#define nvmeib_ntohb(x)	({ _Static_assert(sizeof(x) == 1, "size != 1"); (x); })
#define NVMEIB_HTONLL(x)({ _Static_assert(sizeof(x) == 8, "size != 8"); nvmeib_htonll(x); })
#define NVMEIB_NTONLL(x)({ _Static_assert(sizeof(x) == 8, "size != 8"); nvmeib_ntohll(x); })
#ifdef __KERNEL__
	#define nvmeib_htonll(x) cpu_to_be64(x)
	#define nvmeib_ntohll(x) be64_to_cpu(x)
#elif defined(__BYTE_ORDER__)
	#include <byteswap.h>	// #include <endian.h>
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		static inline uint64_t nvmeib_htonll(uint64_t x) { return bswap_64(x); }
		static inline uint64_t nvmeib_ntohll(uint64_t x) { return bswap_64(x); }
	#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		static inline uint64_t nvmeib_htonll(uint64_t x) { return x; }
		static inline uint64_t nvmeib_ntohll(uint64_t x) { return x; }
	#else
		#error __BYTE_ORDER__ is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
	#endif
#else
	#include <byteswap.h>	// #include <endian.h>
	/* GCC 4.4.7 does not define __BYTE_ORDER__ macros so use architecture,
   https://lists.freedesktop.org/archives/spice-devel/2015-August/021619.html*/
	#if defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) \
		|| defined(__THUMBEL__) || defined(__AARCH64EL__) \
		|| defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) \
		|| defined(__amd64__) || defined(__x86_64__) || defined(__i386__)

		static inline uint64_t nvmeib_htonll(uint64_t x) { return bswap_64(x); }
		static inline uint64_t nvmeib_ntohll(uint64_t x) { return bswap_64(x); }
	#elif defined(__BIG_ENDIAN__) || defined(__ARMEB__) \
		|| defined(__THUMBEB__) || defined(__AARCH64EB__) \
		|| defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__)

		static inline uint64_t nvmeib_htonll(uint64_t x) { return x; }
		static inline uint64_t nvmeib_ntohll(uint64_t x) { return x; }
	#else
		#error __BYTE_ORDER__ is not defined in user space compilation
	#endif
#endif

static inline enum nvmeib_rdma_transport nvmeib_transport_cton(char c)
{
	switch (c) {
	case NVMEIBS_NICS_CSV_TRANSPORT_INFINIBAND:	return rtr_ib;
	case NVMEIBS_NICS_CSV_TRANSPORT_ROCE:		return rtr_roce;
	case NVMEIBS_NICS_CSV_TRANSPORT_TCP:		return rtr_tcp;
	case NVMEIBS_NICS_CSV_TRANSPORT_MULTI:		return rtr_multi;
	default: return rtr_unknown;
	}
}

#define UK_ZERO_TEST 0

static inline char nvmeib_transport_ntoc(enum nvmeib_rdma_transport t)
{
	switch (t) {
	case rtr_ib:	return NVMEIBS_NICS_CSV_TRANSPORT_INFINIBAND;
	case rtr_roce:	return NVMEIBS_NICS_CSV_TRANSPORT_ROCE;
	case rtr_tcp:	return NVMEIBS_NICS_CSV_TRANSPORT_TCP;
	case rtr_multi: return NVMEIBS_NICS_CSV_TRANSPORT_MULTI;
	default:		return NVMEIBS_NICS_CSV_TRANSPORT_UNDEF;
	}
}

// Daniel, Todo: Once Toma stops accessing disks via stock driver, this can be moved to nvmeibs_nvme/c code
static inline int nvmeib_copy_str_trim_spaces(char*dst, const char*src, int DST_LEN, int SRC_LEN)
{	// Assume DST_LEN >= SRC_LEN
	int i;
	for (i = SRC_LEN; ((i > 0) && (src[SRC_LEN-i] == ' ')); (i)--);	// Skip leading spaces
	memcpy(dst, (src+SRC_LEN-i), i);
	for (; i > 0 && dst[i-1] == ' '; i--);							// Cut trailing spaces
	if (i < DST_LEN)
		dst[i] = '\0';												// If possible, add '\0' for printability
	return i;														// return strlen(dst), as if '\0' always exists
}

#define PROC_NAME_LEN 16 // should be equal to TASK_COMM_LEN
#define PROC_NAME_STR_FMT "X%c%-.2s%-.8s/%02d"
#define CLNT_PROC_NAME_STR_FMT "X%c%-.2s%-.7s/%03d"
#define CLNT_PROC_NAME_STR_FMT_EXTD "X%c%-.2s%-.4s/%02d/%02d"
#define SRV_PROC_NAME_STR_FMT "X%c%-.2s%-.8s/%02d"
typedef char proc_name_t[PROC_NAME_LEN];
#define proc_name_format(src, ptype, pdesc) "X" src ptype pdesc
#define proc_name_format_extd(name, src, ptype, pdesc, idx) snprintf(name, sizeof(name), PROC_NAME_STR_FMT, src, ptype, pdesc, idx % 100)
#define clnt_proc_name_format(name, src, ptype, pdesc, cinst) snprintf(name, sizeof(name), CLNT_PROC_NAME_STR_FMT, src, ptype, pdesc, cinst)
#define clnt_proc_name_format_extd(name, src, ptype, pdesc, cinst, inst) snprintf(name, sizeof(name), CLNT_PROC_NAME_STR_FMT_EXTD, src, ptype, pdesc, cinst % 100, inst % 100)
#define srv_proc_name_format(name, src, ptype, pdesc, idx) snprintf(name, sizeof(name), SRV_PROC_NAME_STR_FMT, src, ptype, pdesc, idx)

#define VDISK_PREFIX "vdisk"
#define IS_PATH_VDISK(_path) strstr(_path, VDISK_PREFIX)

#define VDISK_VENDOR 12345
#define VDISK_MODEL "EXCELERO_VDISK"
#define VDISK_UEVENT_IO_ENABLE_KEY "VDISK_UEVENT_IO_ENABLE"
#define VDISK_UEVENT_IO_ENABLE VDISK_UEVENT_IO_ENABLE_KEY "=1"
#define VDISK_UEVENT_IO_DISABLE VDISK_UEVENT_IO_ENABLE_KEY "=0"

#define MAGIC_CONFIG_UPDATE_TOKEN "F0AAAAAAAAAAAAA"			// Use config to only update existing volumes, not attaching new volumes and not changing type of existing volume
#define MAGIC_CONFIG_FORCE__TOKEN "FAAAAAAAAAAAAAA"			// Use config to update/attach allow chaning type of existing. UNSAFE!!!!
#define MAGIC_CONFIG_SHADOW_TOKEN "FBAAAAAAAAAAAAA"			// Used by toma to attach encrypted volume; the volume behaves like usual volume, but reports attach_type=SHADOW in status.* files
#define MAGIC_CONFIG_IGNORE_TOKEN "FBBBBBBBBBBBBBB"			// New Token to replace above token on init_mcs message, will prevent overriding persitency only variables
#define MAGIC_RECOVR_ATTACH_TOKEN "AAAAAAAAAAAAAAA"			// Use config to attach recovery volume or update existing volume. Used for recovery
#define MAGIC_HIDDEN_ATTACH_TOKEN "BBBBBBBBBBBBBBB"			// Use config to attach hidden volume or update existing volume. Used for hidden only


#endif //_NVMEIB_SHARED_
