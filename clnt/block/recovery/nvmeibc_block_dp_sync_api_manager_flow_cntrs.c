/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"					// Must be first for simulator
#include "common/nvmeib_shared.h"
#include "block/recovery/nvmeibc_block_dp_sync_api_manager.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

struct nvmeibc_flow_counters _gsfc = {	// _global_sync_flow_cntrs
	.nowh = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.jour = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.main = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.htrs = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0),
			  ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0),
			  ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
};

struct nvmeibc_flow_counters* nvmeibc_flow_counters_ref(void) { return &_gsfc; }

#define BUF_ADD(...) pos += scnprintf(buf+pos, len-pos, __VA_ARGS__)

/**************************** No write hole stats counter *********************/
void nvmeibc_nowhole_stats_tojson(struct jdr *jdr)
{
	struct nvmeibc_nowhole_stats st;
	jdr_object_scope(jdr, "nowhole");
	nvmeibc_nowhole_stats_get(&st);		// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	jdr_write_var(jdr, n_dbits, atomic_read(&st.n_dbits_fix));
	jdr_write_var(jdr, n_bad_sect, atomic_read(&st.n_bdsec_fix));
	jdr_write_var(jdr, n_roll_back, atomic_read(&st.n_rlbck_fix));
	jdr_write_var(jdr, n_scrub, atomic_read(&st.n_scrub_fix));
	jdr_write_var(jdr, n_destroy, atomic_read(&st.n_destoyed));
	jdr_write_var(jdr, n_di, atomic_read(&st.n_di_fix));
	jdr_write_var(jdr, n_other, atomic_read(&st.n_other_fix));
	jdr_write_var(jdr, n_resets, atomic_read(&st.n_resets));
}

void nvmeibc_nowhole_stats_reset(void)
{
	const int memset_size = (sizeof(_gsfc.nowh)-sizeof(_gsfc.nowh.n_resets));
	atomic_inc(&_gsfc.nowh.n_resets);
	memset(&_gsfc.nowh, 0, memset_size);
}

void nvmeibc_nowhole_stats_get(struct nvmeibc_nowhole_stats *rv)
{
	memcpy(rv, &_gsfc.nowh, sizeof(_gsfc.nowh));
}

/****************************** Journal syncs stats counter ****************************/
void nvmeibc_cold_stats_tojson(struct jdr *jdr)
{
	struct nvmeibc_cold_stats st;
	jdr_object_scope(jdr, "cold");
	nvmeibc_cold_stats_get(&st);	// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	jdr_write_var(jdr, n_syncs, atomic_read(&st.n_syncs));
	jdr_write_var(jdr, n_htrs, atomic_read(&st.n_htr_call));
	jdr_write_var(jdr, n_nowhl, atomic_read(&st.n_no_htr_call));
	jdr_write_var(jdr, n_jgc_freed, atomic_read(&st.n_jgc_freed));
	jdr_write_var(jdr, n_jgc_blksets, atomic_read(&st.n_jgc_blksets));
	jdr_write_var(jdr, n_resets, atomic_read(&st.n_resets));
}

void nvmeibc_cold_stats_reset(void)
{
	const int memset_size = (sizeof(_gsfc.jour)-sizeof(_gsfc.jour.n_resets));
	atomic_inc(&_gsfc.jour.n_resets);
	memset(&_gsfc.jour, 0, memset_size);
}

void nvmeibc_cold_stats_get(struct nvmeibc_cold_stats *rv)
{
	memcpy(rv, &_gsfc.jour, sizeof(_gsfc.jour));
}

/********************** Maintanace syncs stats counter ************************/
void nvmeibc_maintain_sync_stats_tojson(struct jdr *jdr)
{
	struct nvmeibc_maintain_sync_stats st;
	jdr_object_scope(jdr, "maintenance");
	nvmeibc_maintain_sync_stats_get(&st);	// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	jdr_write_var(jdr, n_txid_wrap, atomic_read(&st.n_txid_wrap));
	jdr_write_var(jdr, n_txid_unk, atomic_read(&st.n_txid_resolve));
	jdr_write_var(jdr, n_dbit_unk, atomic_read(&st.n_dbits_resolve));
	jdr_write_var(jdr, n_binfo_unk_readfail, atomic_read(&st.n_binfo_resolve_readfail));
	jdr_write_var(jdr, n_commit_binfo, atomic_read(&st.n_commit_binfo));
	jdr_write_var(jdr, n_dconv_turnon, atomic_read(&st.n_dconvict_turnon));
	jdr_write_var(jdr, n_resets, atomic_read(&st.n_resets));
}

void nvmeibc_maintain_sync_stats_reset(void)
{
	const int memset_size = (sizeof(_gsfc.main)-sizeof(_gsfc.main.n_resets));
	atomic_inc(&_gsfc.main.n_resets);
	memset(&_gsfc.main, 0, memset_size);
}

void nvmeibc_maintain_sync_stats_get(struct nvmeibc_maintain_sync_stats *rv)
{
	memcpy(rv, &_gsfc.main, sizeof(_gsfc.main));
}

/****************************** HTR stats counter ****************************/
void nvmeibc_htr_status_tojson(struct jdr *jdr)
{
	const struct htr_stats *s = &_gsfc.htrs;
	jdr_object_scope(jdr, "htr");
	jdr_write_var(jdr, n_calls, atomic_read(&s->n_calls));
	jdr_write_var(jdr, n_comps_ok, atomic_read(&s->n_comps));
	jdr_write_var(jdr, n_comps_err, atomic_read(&s->n_comps_err));
	jdr_write_var(jdr, n_uuid_no_jour, atomic_read(&s->n_uuid_no_jour));
	jdr_write_var(jdr, n_jour_cmtd, atomic_read(&s->n_jour_cmtd));
	jdr_write_var(jdr, n_data_cmtd, atomic_read(&s->n_data_cmtd));
	jdr_write_var(jdr, n_roll_fwd, atomic_read(&s->n_roll_fwd));
	jdr_write_var(jdr, n_roll_bkw_by_dbits_turnon, atomic_read(&s->n_roll_bkw_by_dbits_turnon));
	jdr_write_var(jdr, n_roll_fwd_by_dbits_turnon, atomic_read(&s->n_roll_fwd_by_dbits_turnon));
	jdr_write_var(jdr, n_regen_fwd, atomic_read(&s->n_regen_fwd));
	jdr_write_var(jdr, n_regen_bkw, atomic_read(&s->n_regen_bkw));
	jdr_write_var(jdr, n_update_parity_dbits, atomic_read(&s->n_update_parity_dbits));
	jdr_write_var(jdr, n_send_recov, atomic_read(&s->n_send_recovered));
	jdr_write_var(jdr, n_regen_bkw_no_pari, atomic_read(&s->n_regen_bkw_no_pari));
	jdr_write_var(jdr, n_dbits_turnon_no_pari, atomic_read(&s->n_dbits_turnon_no_pari));
	jdr_write_var(jdr, n_colds, atomic_read(&s->n_colds));
	jdr_write_var(jdr, n_dbits_rebuild, atomic_read(&s->n_dbits_rebuild));
	jdr_write_var(jdr, n_resets, atomic_read(&s->n_resets));
}

void nvmeibc_htr_stats_reset(void)
{
	const int memset_size = (sizeof(_gsfc.htrs)-sizeof(_gsfc.htrs.n_resets));
	atomic_inc(&_gsfc.htrs.n_resets);
	memset(&_gsfc.htrs, 0, memset_size);
}

void nvmeibc_htr_stats_get(struct htr_stats *rv)
{
	memcpy(rv, &_gsfc.htrs, sizeof(_gsfc.htrs));
}
