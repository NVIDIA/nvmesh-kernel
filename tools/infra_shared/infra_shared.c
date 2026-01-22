/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "common/kr_incs.h" /*Must be first*/
#include "srv/nvmeibs_srv_toma_messages.h"
#include "clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h"
#include "clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_print.h"
#include "nvmeib_shared.h"
#include "nvmeib_types.h"
#include <linux/netlink.h>
#include "common/compat/kr_incs_crc32.inc.c"

struct nvmeib_get_disk_names_reply dummy1;
struct nlmsghdr dummy2;
struct nvmeib_nl_uk_comm_msg dummy3;
struct nvmeib_nl_uk_comm_rep dummy4;
struct nvmeib_io_to_disk dummy5;
struct t_data_blk dummy6;
enum uk_comm_opcode dummy7;
struct nvmeib_io_to_disk_reply dummy8;

/*
 * A list of c functions needed by Infra
 */
u64 get_j2d(union jblock_md *md)
{
	return nvmeibc_block_dp_ec_jmd_decode_j2d_only(md);
}

void set_j2d(union jblock_md *md, u64 j2d)
{
	nvmeibc_block_dp_ec_jmd_encode_j2d_only(md, j2d);
}

u32 calc_edic_from_rlba_and_data(u64 rlba, const unsigned char *data, bool debug_di_enabled)
{
	#if 0
		return nvmeibc_calculate_edic_from_data_and_rlba(rlba, data, debug_di_enabled);		// Reimplement to avoid gf inclusion in infra.so
	#else
		const u32 salt = crc32c(~0U, (const unsigned char *)&rlba, sizeof(rlba));
		const int size = DEBUG_DI_SIZE_CALC(debug_di_enabled);
		return ~crc32c(salt, data, size);
	#endif
}

bool infra_was_data_never_written(const union nvmeibc_block_dp_ec_data_block_md *md)
{
	return nbdpec_md_was_data_never_written(md);
}

int infra_has_problems_in_block(u64 rlba, const unsigned char *data, bool debug_di_enabled, const bool is_parity, const union nvmeibc_block_dp_ec_data_block_md *md)
{	// Return values: 0 - means OK. 'V' for virgin data (never written), 'Z' for logical zero (regardless of block content it is zero), 'B' for bad sector, 'C' for wrong crc check
	const enum nvmeibc_data_written_state md_state = nbdpec_md_get_data_written_state(md);
	const char *md_state_str = nvmeibc_data_written_state_tostring(md_state);
	if (md_state != DATA_WRITTEN) {
		return (int)md_state_str[0];
	} else {
		const u32 crc_mask = NVMEIBC_DP_EC_MD_EDIC_MASK(is_parity);
		const u32 edic_calc = calc_edic_from_rlba_and_data(rlba, data, debug_di_enabled);
		const u32 edic_md = (u32)(is_parity ? md->P.edic : md->D.edic);
		const bool edic_ok = (((edic_md ^ edic_calc) & crc_mask) == 0);
		return (edic_ok ? 0 : 'C');
	}
	// If need a fix use nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck...() methods or cmp_blocks utility
}
