#ifndef NVMEIB_VOLUME_TYPE_H
#define NVMEIB_VOLUME_TYPE_H

#include "nvmeib_math.h"
enum nvmeibc_config_volume_type {					// List of flags
	NORMAL_VOLUME =            0x0,					// Regular (thick) block device as implemented since nvmesh 2.0
	// -------------- generic flags, can combine with any other  -------------
	RECOVERER_VOLUME =         0x1,					// Recoverer volume, ignores all reservation info, only for recovery purposes, will be auto-detach if no recoveries are running,Usually is also hidden but not a must
	HIDDEN_VOLUME =		       0x2,					// Hidden Volume. No kernel IO allowed, unlike recoverer hidden volume is not auto-detached
	AUTO_EXTEND_VOLUME =       0x4,					// Block device which acts as thin provisioned auto growing volume. Allow writes to tange > max_vlba
	SUBBLOCK_DCOW_VOLUME =     0x8,					// Supports writes (compare-and-write) or partial blocks (read-modify-write)
	SHADOW_VOLUME =            0x10,				// Shadow(encrypted) volume, created by Toma for a short period of time; should behave like normal volume
													// Except: status.* files should report it as SHADOW & should NOT send keep alive report to management
	ALL_GENERIC_FLAGS_VOLUME = 0x1F,					// All of the above
	// -------------- Inter block-dev communication flags -------------
	CARRIER_D_VOLUME =        0x20 /*| RECOVERER_VOLUME*/,	// Block device which acts as data carrier for. By default is hidden
	CARRIER_MD_VOLUME =       0x40 /*| RECOVERER_VOLUME*/ | SUBBLOCK_DCOW_VOLUME,	// Block device which acts as metadata carrier for normal  volume . By default is hidden
	// resrved =              0x80,
	CARRIER_VOLUME =          0xE0,						// Any of the above, Is non-Virtual non-Normal volume

	RIDER_MD_VOLUME =        0x100,						// Block device which stores its metadata in CARRIER_MD_VOLUME
	RIDER_D_VOLUME =         0x200,						// Block device which stores its data     in CARRIER_D_VOLUME
	// resrved =             0x800,
	RIDER_VOLUME =           0xF00,						// Any type of rider volume
	// ------------- most commonly used combinations ------
	//QLCEC_VOLUME =          0x1000 | CARRIER_D_VOLUME | RIDER_MD_VOLUME,	// Block device which stores its metadata in CARRIER_MD_VOLUME and acts as CARRIER_D_VOLUME for upper layer
	//WCV_VOLUME = CARRIER_D_VOLUME | SUBBLOCK_DCOW_VOLUME, //(| RIDER_MD_VOLUME? Can WCV live without MDV?)
	// MDV_VOLUME = CARRIER_MD_VOLUME,
	//MULTIER_VOLUME =        0x2000 | RIDER_D_VOLUME | RIDER_MD_VOLUME,	// Multi-tiered volume. Rides on top of: {CARRIER_D_VOLUME, CARRIER_MD_VOLUME, QLCEC_VOLUME}
	//MULTIER_HIDDEN_VOLUME = MULTIER_VOLUME | RECOVERER_VOLUME,
	// resrved =            0x4000,
	ALLBITS_IN_TYPEVOLUME = 0xFFFF,
	UNKNOWN_ILLEGAL =     0xBADFAC,					// Illegal type of volume, can be inserted to func's which search volumes to search all volume types
};

static inline bool __is_vol_type_valid(enum nvmeibc_config_volume_type t)
{	// Check for mutual exclusive flags
	const uint32_t c0 = (CARRIER_VOLUME);				// Can carry only 1 type of information
	const uint32_t c1 = (RIDER_MD_VOLUME | CARRIER_MD_VOLUME);
	const uint32_t c2 = (RIDER_D_VOLUME  | CARRIER_D_VOLUME);
	const uint32_t c4 = (ALLBITS_IN_TYPEVOLUME & (~(CARRIER_VOLUME|RIDER_VOLUME)));		// Can have only 1 major type
	t &= (~ALL_GENERIC_FLAGS_VOLUME);				// Remove generic flags which of no interest here
	if (hweight32(t & c0) > 1) return false;
	if (hweight32(t & c1) > 1) return false;
	if (hweight32(t & c2) > 1) return false;
	if (hweight32(t & c4) > 1) return false;
	if (t &(~ALLBITS_IN_TYPEVOLUME)) return false;	// Wrong bits
	return true;
}

#define nvmeibc_block_is_recoverer(v) (                    RECOVERER_VOLUME & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_hidden(v) (         	           HIDDEN_VOLUME    & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_shadow(v) (         	           SHADOW_VOLUME    & (v)->type)		// 1 Bit flag
#define nvmeibc_block_is_any_carrier(v) (                  CARRIER_VOLUME   & (v)->type)		// At least 1 flag from mask
#define nvmeibc_block_is_any_rider(v) (                    RIDER_VOLUME     & (v)->type)		// At least 1 flag from mask
#define nvmeibc_block_is_rider_md(v) (                     RIDER_MD_VOLUME  & (v)->type)		// At least 1 flag from mask
#define nvmeibc_block_is_rider_d(v) (                      RIDER_D_VOLUME   & (v)->type)		// At least 1 flag from mask
#define nvmeibc_block_is_d_carrier(v) __is_bmp_included_in(CARRIER_D_VOLUME,  (v)->type)
#define nvmeibc_block_is_recoverer_or_hidden(v) ((HIDDEN_VOLUME|RECOVERER_VOLUME) & (v)->type)	// Todo: Remove in future. Back-compatible for 2.2.0 and below + Old managements. Mgmt cares about this condition.

#endif /* NVMEIB_VOLUME_TYPE_H */
