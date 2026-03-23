#ifndef NVMEIBC_DP_BINFO_COMMON_H
#define NVMEIBC_DP_BINFO_COMMON_H
/* Handle ram binfo of blockset.
 */
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "block/nvmeibc_block_common.h"

static inline bool nvmeibcbdp_binfo_has_txid_unreslved(const union nvmeib_blkset_info bi)
{
	return (bi.bits.txid == INITIAL_LAZY_READ_TXID);
}

static inline bool nvmeibcbdp_binfo_has_txid_unreslvd(const struct nvmeibc_block_command *rldr)
{
	return nvmeibcbdp_binfo_has_txid_unreslved(rldr->rld.pre);
}

/* We have at least 1 unknown dbits, and cannot resolve them from disk, assume worst case (by topo) */
union nvmeibc_dbits_entry nvmeibcbdp_binfo_calc_worst_case_dbits_in_topology(
	const union nvmeibc_dbits_entry pre, const struct nvmeibc_raid1 *pr);

static inline bool nvmeibcbdp_binfo_has_unknown_dbits(const struct nvmeibc_block_command *rldr, const struct nvmeibc_raid1 *pr)
{
	const union nvmeibc_dbits_entry pre = {.all_bits = rldr->rld.pre.bits.dirty};
	return nvmeibc_dbits_get_n_unk(&pre, &pr->calculated_data.topo_traits);
}

// Todo: Move the functions below to here
//bool dp_sync_common_are_all_binfo_equal(....)
//bool dp_sync_common_has_dbits_anywhere(....)

#endif  // H beginning

