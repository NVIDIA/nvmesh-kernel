/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "nvmeib_shared.h"	// Just for N_MAX_RAID_SLICE_LEN
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"
#include "nvmeib_math.h"	//	__is_bmp_included_in

/************************** CRC functionality *********************************/
// RFC for CRC32C forces us to use ~0 as initial value, and final value must
// do ~ before storing CRC into MD
u32 nvmeibc_rlba_to_salt(const u64 rlba)
{
	return (ec_crc(~0U, (const unsigned char *)&rlba, sizeof(rlba)));
}

// Utiliy function for when we want to verify read edic
u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled)
{
	const u32 salt = nvmeibc_rlba_to_salt(rlba);
	const int size = DEBUG_DI_SIZE_CALC(debug_di_enabled);
	return ~ec_crc(salt, data, size);
}

int nvmeibc_reed_solomon_fill_missing(const int replicas, const int slice_size, const int snake_size,
									  unsigned char *vec[N_MAX_RAID_SLICE_LEN],
									  unsigned char *cpy_vec[N_MAX_RAID_SLICE_LEN],
									  u32 *crc[N_MAX_RAID_SLICE_LEN],
									  const u32 in_bm, const u32 out_bm,
									  const u32 crc_bm, const u64 first_rlba,
									  const bool is_di_debug)
{
	const int parities =  replicas - slice_size;
	const u32 data_mask = GENMASK(slice_size - 1, 0);
	bool data_restoration = (out_bm & data_mask);
	const bool parity_calculation = (out_bm >> slice_size);
	const int  size = DEBUG_DI_SIZE_CALC(is_di_debug);
	int rv = 0;

	u32 tmp_out_bm = out_bm;
	WARN(data_restoration && !__is_bmp_included_in(crc_bm, out_bm), "D+P{%d+%d}, bm={crc=0x%x, in=0x%x, out=0x%x}", slice_size, replicas - slice_size, crc_bm, in_bm, out_bm);
	if (!data_restoration && ((in_bm ^ data_mask) & data_mask)) { // Missing input not in output
		tmp_out_bm |= ((in_bm ^ data_mask) & data_mask);
		data_restoration = true;
	}
	if (data_restoration) { // Restore data
		int i, new_data_index = 0;
		u32 curr;
		u8 * new_data[N_MAX_RAID_SLICE_LEN];
		u8 * data[    N_MAX_RAID_SLICE_LEN];
		u32 _crc[     N_MAX_RAID_SLICE_LEN];
		for (i = 0, curr = 1;i < replicas;i++, curr <<= 1) {
			if (curr & in_bm) { 		// Set all valid inputs
				data[i] = vec[i];
				BUG_ON(data[i] == NULL);
			} else if (curr & tmp_out_bm) { // Set all output requests
				data[i] = NULL;
				new_data[new_data_index] = vec[i];
				if (!parity_calculation && curr & crc_bm) {	// Add crc if required
					const u64 srlba = first_rlba + ((i < slice_size) ? (i*snake_size) : 0);
					_crc[new_data_index++] = nvmeibc_rlba_to_salt(srlba);
				} else _crc[new_data_index++] = 0; // We don't really care for it
			} else { // Not required to be calculated (nor CRC)
				data[i] = (void *)(-1);
				//new_data[new_data_index] = NULL;
				//_crc[new_data_index++] = 0;
				BUG_ON(!parity_calculation && (curr & crc_bm)); // should we allow this without the data?
			}
		}
		if (ec_decode_data(NULL, size, parities, slice_size, data, new_data, _crc))
			rv = -EIO;
		for (i = 0, curr = 1, new_data_index = 0;i < replicas && !parity_calculation;i++, curr <<= 1) { // Crc result required ~ before assinging
			if (curr & crc_bm) *crc[i] = ~_crc[new_data_index++];
			else if (curr & out_bm)            new_data_index++;
		}
	}
	if (parity_calculation) { // Calculate Parity
		int i, new_parity_index = 0;
		u32 curr;
		u8 * coding[         N_MAX_RAID_SLICE_LEN];
		u8 * data[           N_MAX_RAID_SLICE_LEN];
		u32 _crc[N_MAX_RAID_SLICE_LEN];
		for (i = 0, curr = 1;i < replicas;i++, curr <<= 1) {
			if (i < slice_size) {	// Set all valid inputs (we might have just generated it's data)
				data[i] = vec[i];
				BUG_ON(data[i] == NULL);
				if (curr & crc_bm) { // Set salt to CRC
					const u64 srlba = first_rlba + i*snake_size;
					_crc[i] = nvmeibc_rlba_to_salt(srlba);
				} else { // Do we even care?
					_crc[i] = 0;
				}
			} else if (curr & out_bm) { // Set all output requests
				coding[new_parity_index++] = vec[i];
				if (curr & crc_bm) {	// Add crc if required
					_crc[i] = nvmeibc_rlba_to_salt(first_rlba);
				} else _crc[i] = 0; // We don't really care for it
			} else { // Not required to be calculated (nor CRC)
				coding[new_parity_index++] = NULL;
				_crc[i] = 0;
				BUG_ON(curr & crc_bm); // should we allow this without the data?
			}
		}
		if (ec_encode_data(NULL, size, parities, slice_size, data, coding, _crc, cpy_vec))
			rv = -EIO;
		for (i = 0, curr = 1;i < replicas;i++, curr <<= 1) { // Crc result required ~ before assinging
			if (curr & crc_bm) *crc[i] = ~_crc[i];
		}
	}
	return rv;
}

int nvmeibc_reed_solomon_update_parities(const int replicas, const int slice_size, const int snake_size,
										 unsigned char *old_vec[N_MAX_RAID_SLICE_LEN],
										 unsigned char *new_vec[N_MAX_RAID_SLICE_LEN],
										 unsigned char *cpy_vec[N_MAX_RAID_SLICE_LEN],
										 u32 *crc[N_MAX_RAID_SLICE_LEN],
										 const u32 old_bm, const u32 crc_bm,
										 const u64 first_rlba,
										 const bool is_di_debug)
{
	const int parities =  replicas - slice_size;
	const u32 data_mask = GENMASK(slice_size - 1, 0);
	const ulong data_update = (ulong)(old_bm & data_mask);
	const int  size = DEBUG_DI_SIZE_CALC(is_di_debug);
	int i, j;
	bool first_iteration = true;
	u32 _crc[N_MAX_RAID_SLICE_LEN + 1];
	int calc_rv = 0;

	for_each_set_bit(i, &data_update, slice_size) {
		u8 * data[N_MAX_RAID_SLICE_LEN + 2];
		u8 * coding[N_MAX_RAID_SLICE_LEN];

		// Set old and new data
		data[0] = old_vec[i];
		data[1] = new_vec[i];

		if ((1 << i) & crc_bm) { // Set data CRC
			_crc[0] = nvmeibc_rlba_to_salt(first_rlba + i*snake_size);
		} else {
			_crc[0] = 0;
		}

		if (first_iteration) {	// old Parities - only in first iteration
			for (j = 0; j < parities; j++) {
				const u32 curr = (1 << (slice_size + j));
				if (curr & old_bm) {	// Update parity
					data[2 + j] = old_vec[slice_size + j];
					coding[j] =   new_vec[slice_size + j];
				} else {			 	// Do not update parity TODO (Doron) check if allowed, since reed solomon fails on it
					data[2 + j] = NULL;
					coding[j] = NULL;
				}
				if (curr & crc_bm) {	// Set CRC salt for this iteration
					_crc[1 + j] = nvmeibc_rlba_to_salt(first_rlba);
				} else {				// Do not care about CRC
					_crc[1 + j] = 0;
				}
			}
			// Next iterations will use the new_vec (paritally calculated parites)
			first_iteration = false;
		} else {				// new Parities - stored after each iteration
			for (j = 0; j < parities; j++) {
				const u32 curr = (1 << (slice_size + j));
				if (curr & old_bm) {	// Update parity
					coding[j] = data[2+j] = new_vec[slice_size+j];
				} else {				// Do not update parity
					coding[j] = data[2+j] = NULL;
				}
				if (curr & crc_bm) {	// Set CRC salt for this iteration
					_crc[1 + j] = nvmeibc_rlba_to_salt(first_rlba);
				} else {				// Do not care about CRC
					_crc[1 + j] = 0;
				}
			}
		}
		if (ec_encode_data_update(NULL, size, parities, i, data, coding, _crc, cpy_vec[i]))
			calc_rv = -EIO;

		// Store CRC when required
		if ((1 << i) & crc_bm) {
			*crc[i] = ~_crc[0];
		}

		// Store latest parity into new_vec
		for (j = 0; j < parities; j++) {
			const u32 curr = (1 << (slice_size + j));
			if (curr & old_bm) {
				new_vec[slice_size+j] = coding[j];
			}
		}
	}

	// Done with all updates store CRC of final parity when requried
	for (j = 0; j < parities; j++) {
		const u32 curr = (1 << (slice_size + j));
		if (curr & crc_bm) {
			*crc[slice_size + j] = ~_crc[1 + j];
		}
	}

	return calc_rv;
}
