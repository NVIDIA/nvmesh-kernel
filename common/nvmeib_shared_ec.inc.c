/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_shared_ec_inc_c

bool nvmeib_jentry_md_is_valid(const struct jentry_md *jentry, const u32 binje) {
	struct jblock_md_decompressed first, cur;
	union jblock_md *md = &jentry->md_arr[0];
	u32 jb;
	if (!nvmeib_is_jmd_io_entry(*md))
		return false;									// Journal-never-written means no chain
	if (md->version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED)	// Does not support chain,
		return true;
	first = nvmeibc_block_dp_ec_jmd_decode(md);

	WARN(jblock_md_prev_link(md), "nvmeibc bug!, first jblock in the jentry must not have prev, j2d=%llu, txid=%u, txbm=%u\n", first.j2d, first.tx_id, first.tx_bmp);
	for (jb = 1; (jb < binje) && (jentry->md_arr[jb-1].v1.has_next); jb++) {
		md = &jentry->md_arr[jb];
		cur = nvmeibc_block_dp_ec_jmd_decode(md);
		if (!nvmeib_is_jmd_io_entry_by_txid(md->tx_id))
			return false;								// Previous said that at least one other jblock exists.
		if (first.version != cur.version) {
			WARN(true, "nvmeibc bug!, version in the entry is not equal: version_first=%u version_other=%u index=%d\n", first.version, cur.version, jb);
			return false;								// Version changed, this is definitely not a chain.
		}												// Versions match, safe to test other fields
		if (0) { // !md->v1.has_prev) {
			WARN(true, "nvmeibc bug!, jblock in the middle of jentry must have prev, j2d=%llu, txid=%u, txbm=%u\n", cur.j2d, cur.tx_id, cur.tx_bmp);
			return false;
		}
		if (cur.j2d != (first.j2d+jb) || (cur.tx_id != first.tx_id)) // J2D and TxID dont match, Probably IO was aborted during chain write
			return false;
	}
	if (unlikely(jentry->md_arr[jb-1].v1.has_next)) {	// Last entry still points to next
		md = &jentry->md_arr[jb];
		cur = nvmeibc_block_dp_ec_jmd_decode(md);
		WARN(true, "nvmeibc bug!, can't be possible that the entry in offset binje has_next, j2d=%llu, txid=%u, txbm=%u\n", cur.j2d, cur.tx_id, cur.tx_bmp);
		return false;
	}
	return true;
}

void nvmeibc_jentry_md_container_fill(struct jentry_md_container *dst, struct jentry_md *src,
	u32 sw2hw, u32 n, binje_t binje)
{
	u32 j;
	BUG_ON(n > binje);
	for (j = 0; j < n; j++)
		dst->jblks_md[j].raw = src->md_arr[j << sw2hw].raw;
	for (; j < binje; j++)
		dst->jblks_md[j].raw = nvmeib_jmd_unused_jblock_val.raw;
}

#pragma pop_macro("__FILE_LITERAL__")

