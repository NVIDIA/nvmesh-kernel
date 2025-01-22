#include "nvmeibc_block_dp_binfo.h"

union nvmeibc_dbits_entry nvmeibcbdp_binfo_calc_worst_case_dbits_in_topology(
	const union nvmeibc_dbits_entry pre, const struct nvmeibc_raid1 *pr) {
	struct nvmeibc_dbits_tx tx;
	const sgmnts_bmp_t turn_on_dbit_bmp = nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_off_mask) | nvmeibc_raid1_get_sgmnts_bmp(pr, dbits_on_mask);
	const sgmnts_bmp_t turn_on_conv_bmp = nvmeibc_raid1_get_sgmnts_bmp(pr, wm);
	nvmeibc_dbits_tx_init_by_bmp(&tx, nvmeibc_raid1_get_protect_lvl(pr), turn_on_dbit_bmp, 0 /* turn_off_dbit_bmp */, turn_on_conv_bmp);
	nvmeibc_dbits_tx_apply(&pre, &tx);
	return tx.post;
}
