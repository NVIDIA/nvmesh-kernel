#include "common/kr_incs.h"
#include "nvmeib_shared.h"
#include "nvmeib_types.h"
#include "clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"

roles_pair_compressed_t roles_comp_table[N_MAX_RAID_SLICE_LEN][N_MAX_RAID_SLICE_LEN] = {{0}};
#define DECOMP_TABLE_SIZE (N_MAX_RAID_SLICE_LEN*N_MAX_RAID_SLICE_LEN + NVMEIBC_DP_EC_MD_N_RESERVED_TXBM_VALUES)
struct decomp_entry roles_decomp_table[DECOMP_TABLE_SIZE] = {[0 ... DECOMP_TABLE_SIZE-1] = {.h=0,.l=1}};		// All elements in array yield TxBM=0
/* The table after init:
	 1  2  4  7 11 16 22 29 37 46 56 67
	-1  3  5  8 12 17 23 30 38 47 57 68
	-1 -1  6  9 13 18 24 31 39 48 58 69
	-1 -1 -1 10 14 19 25 32 40 49 59 70
	-1 -1 -1 -1 15 20 26 33 41 50 60 71
	-1 -1 -1 -1 -1 21 27 34 42 51 61 72
	-1 -1 -1 -1 -1 -1 28 35 43 52 62 73
	-1 -1 -1 -1 -1 -1 -1 36 44 53 63 74
	-1 -1 -1 -1 -1 -1 -1 -1 45 54 64 75
	-1 -1 -1 -1 -1 -1 -1 -1 -1 55 65 76
	-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 66 77
	-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 78   */
void init_roles_pair_compression_tables(void) {
	raid_role_t i, j;
	u16 val;

	BUILD_BUG_ON(NVMEIBC_DP_EC_MD_TXBM_INVALID!=0);	// 0 txbm is invalid and always remaines zero
	val = NVMEIBC_DP_EC_MD_N_RESERVED_TXBM_VALUES;  // 0 is reserved value
	for (j = 0; j < N_MAX_RAID_SLICE_LEN; ++j) {
		for (i = 0; i <= j; i++) {
			roles_comp_table[i][j] = val;
			WARN((val == 0) || (val >= DECOMP_TABLE_SIZE), "nvmeibc bug, value larger/equals than decompression table size val=%d\n", val); //sanity
			roles_decomp_table[val].l = i;
			roles_decomp_table[val].h = j;
			++val;
		}
	}

	return;
}