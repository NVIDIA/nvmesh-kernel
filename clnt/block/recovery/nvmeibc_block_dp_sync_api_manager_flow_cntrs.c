#include "kr_incs.h"					// Must be first for simulator
#include "common/nvmeib_shared.h"
#include "block/recovery/nvmeibc_block_dp_sync_api_manager.h"

struct nvmeibc_flow_counters _gsfc = {	// _global_sync_flow_cntrs
	.nowh = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.jour = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.main = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
	.htrs = { ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0),
			  ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0),
			  ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0) },
};

struct nvmeibc_flow_counters* nvmeibc_flow_counters_ref(void) { return &_gsfc; }

#define BUF_ADD(...) pos += scnprintf(buf+pos, len-pos, __VA_ARGS__)

/**************************** No write hole stats counter *********************/
int nvmeibc_nowhole_stats_to_string(char* buf, int len)
{
	ssize_t pos = 0;
	struct nvmeibc_nowhole_stats st;
	nvmeibc_nowhole_stats_get(&st);		// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	BUF_ADD("\"nowhole\":{\n");
	BUF_ADD("\t\"n_dbits\": %u, \"n_bad_sect\": %u, \"n_roll_back\": %u, \"n_scrub\": %u, \"n_destroy\": %u, \"n_di\": %u, \"n_other\": %u, \"n_resets\": %u\n}",
			atomic_read(&st.n_dbits_fix), atomic_read(&st.n_bdsec_fix), atomic_read(&st.n_rlbck_fix), atomic_read(&st.n_scrub_fix),
			atomic_read(&st.n_destoyed), atomic_read(&st.n_di_fix), atomic_read(&st.n_other_fix), atomic_read(&st.n_resets));
	return pos;
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
int nvmeibc_cold_stats_to_string(char* buf, int len)
{
	ssize_t pos = 0;
	struct nvmeibc_cold_stats st;
	nvmeibc_cold_stats_get(&st);	// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	BUF_ADD("\"cold\":{\n");
	BUF_ADD("\t\"n_syncs\": %u, \"n_htrs\": %u, \"n_nowhl\": %u, \"n_jgc_freed\": %u, \"n_jgc_blksets\": %u, \"n_resets\": %u\n}",
			atomic_read(&st.n_syncs), atomic_read(&st.n_htr_call), atomic_read(&st.n_no_htr_call),
			atomic_read(&st.n_jgc_blksets), atomic_read(&st.n_jgc_freed), atomic_read(&st.n_resets));
	return pos;
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
int nvmeibc_maintain_sync_stats_to_string(char* buf, int len)
{
	ssize_t pos = 0;
	struct nvmeibc_maintain_sync_stats st;
	nvmeibc_maintain_sync_stats_get(&st);	// Copy aside to stack to take a snopshot. Live stats can change while this function is running
	BUF_ADD("\"maintenance\":{\n");
	BUF_ADD("\t\"n_txid_wrap\": %u, \"n_txid_unk\": %u, \"n_dbit_unk\": %u,\n", atomic_read(&st.n_txid_wrap), atomic_read(&st.n_txid_resolve), atomic_read(&st.n_dbits_resolve));
	BUF_ADD("\t\"n_commit_binfo\": %u, \"n_dconv_turnon\": %u, \"n_resets\": %u\n}", atomic_read(&st.n_commit_binfo), atomic_read(&st.n_dconvict_turnon), atomic_read(&st.n_resets));
	return pos;
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
int nvmeibc_htr_fill_status(char* buf, int len)
{
	const struct htr_stats *s = &_gsfc.htrs;
	int pos = 0;
	BUF_ADD("\"htr\":{\n");
	BUF_ADD("\t\"n_calls\": %u, \"n_comps_ok\": %u, \"n_comps_err\" : %u, \"n_uuid_no_jour\" : %u,\n",
			atomic_read(&s->n_calls), atomic_read(&s->n_comps), atomic_read(&s->n_comps_err), atomic_read(&s->n_uuid_no_jour));
	BUF_ADD("\t\"n_jour_cmtd\": %u, \"n_data_cmtd\": %u, \"n_roll_fwd\": %u,\"n_roll_bkw_by_dbits_turnon\": %u,\"n_roll_fwd_by_dbits_turnon\": %u,\n",
			atomic_read(&s->n_jour_cmtd), atomic_read(&s->n_data_cmtd), atomic_read(&s->n_roll_fwd), atomic_read(&s->n_roll_bkw_by_dbits_turnon), atomic_read(&s->n_roll_fwd_by_dbits_turnon));
	BUF_ADD("\t\"n_regen_fwd\": %u, \"n_regen_bkw\": %u, \"n_update_parity_dbits\": %u, \"n_send_recov\": %u,\"n_regen_bkw_no_pari\": %u,\"n_dbits_turnon_no_pari\": %u,\n",
			atomic_read(&s->n_regen_fwd), atomic_read(&s->n_regen_bkw), atomic_read(&s->n_update_parity_dbits), atomic_read(&s->n_send_recovered), atomic_read(&s->n_regen_bkw_no_pari), atomic_read(&s->n_dbits_turnon_no_pari));
	BUF_ADD("\t\"n_colds\": %u, \"n_dbits_rebuild\": %u, \"n_resets\": %u\n}",
			atomic_read(&s->n_colds), atomic_read(&s->n_dbits_rebuild), atomic_read(&s->n_resets));
	return pos;
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
