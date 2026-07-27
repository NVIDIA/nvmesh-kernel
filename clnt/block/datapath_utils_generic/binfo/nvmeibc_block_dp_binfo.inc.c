/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "nvmeibc_block_dp_binfo.h"

union nvmeibc_dbits_entry nvmeibcbdp_binfo_calc_worst_case_dbits_in_topology(
	const union nvmeibc_dbits_entry pre, struct dp_topology_traits const* topo_traits) {
	struct nvmeibc_dbits_tx tx;
	const sgmnts_bmp_t turn_on_dbit_bmp = topo_traits->dbits_off | topo_traits->dbits_on;
	const sgmnts_bmp_t turn_on_conv_bmp = topo_traits->wm;
	nvmeibc_dbits_tx_init_by_bmp(&tx, topo_traits, turn_on_dbit_bmp, 0 /* turn_off_dbit_bmp */, turn_on_conv_bmp);
	nvmeibc_dbits_tx_apply(&pre, &tx);
	nvmeibc_dbits_del_unk(&tx.post, topo_traits);
	return tx.post;
}
