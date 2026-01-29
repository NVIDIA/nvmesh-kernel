/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_EC_GF_PRAID_H
#define NVMEIBC_DP_EC_GF_PRAID_H

/******************************** CRC functions *******************************/
u32 nvmeibc_rlba_to_salt(const u64 rlba);
u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data,
											  const bool debug_di_enabled);

/******************************** EC Single Block API *************************/
/* General API for reed solomon
   Inputs: 	Number of segements (replicas) D+P
    		Slice size (D)
    		In/Out Vector for D+P
    		CRC vector for output
    		Valid input bit map (in_bm)
    		Requested data output bitmap (out_bm) (D+P)
    		Requested crc  output bitmap (crc_bm) (D+P)
    		CRC salt first rlba of slice (first_rlba)
    		The DP object (used for debug_di)

    This function will determine what is required:
    1. If needed Data restoration will happen first (with CRC for resotred data if needed)
    2. If needed Parity calculation will happen (with CRC for all data and all parity)

    The Calcualted Data/Parity will be placed within it's respecful entry in the in/out vector
    same thing with CRC */
int nvmeibc_reed_solomon_fill_missing(const int replicas, const int slice_size, const int snake_size,
									  unsigned char *vec[N_MAX_RAID_SLICE_LEN],
									  unsigned char *cpy_vec[N_MAX_RAID_SLICE_LEN],
									  u32 *crc[N_MAX_RAID_SLICE_LEN],
									  const u32 in_bm, const u32 out_bm,
									  const u32 crc_bm, const u64 first_rlba,
									  const bool is_di_debug);

/* Update API for reed solomon
   Inputs: 	Number of segements (replicas) D+P
    		Slice size
    		Old data/parity Vector for D+P (Allowing NULL for unnecessary data)
			New data/parity Vector for D+P (Must match old in data, new parity will be set in this vector)
    		CRC vector for output will be generated for each new data and for the final parity
    		Valid input/output bit map (old_bm, for each set bit data must not be NULL, allowing some paritues to be NULL, nut MUST have at least 1 parity bit set here)
    		CRC Bitmap to know which CRCs are required
			CRC salt first rlba of slice (first_rlba)
    		The DP object (used for debug_di)

    This function will do the following for each set bit:
    1. Set old and new data into the GF function
    2. Set old parity into GF function
    3. Set required CRCs (with salt) into GF function
    4. Store new parities into new_vec and CRC when required

    After the first iteration "old parities" will be the new parities
    After the final iteration CRC of parities can be stored */
int nvmeibc_reed_solomon_update_parities(const int replicas, const int slice_size, const int snake_size,
										 unsigned char *old_vec[N_MAX_RAID_SLICE_LEN],
										 unsigned char *new_vec[N_MAX_RAID_SLICE_LEN],
										 unsigned char *cpy_vec[N_MAX_RAID_SLICE_LEN],
										 u32 *crc[N_MAX_RAID_SLICE_LEN],
										 const u32 old_bm, const u32 crc_bm,
										 const u64 first_rlba,
										 const bool is_di_debug);


#endif  // NVMEIBC_DP_EC_GF_PRAID_H

