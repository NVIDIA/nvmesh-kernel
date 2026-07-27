#include "./cmp_blocks_impl.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"
#include <stdarg.h>

struct cmp_blks_archive summary = { .buf = NULL, .count = 0, .len = 0 };
static struct context_t {
	u64 rlba;
	bool had_errors;
	int verbose_level;
	u32 sum_buf_ind_of_retval;	// Pointer into summary buf
} ctx = {.rlba = 0UL, .had_errors = false, .verbose_level = 1, 0};

const char *retval_string[] = {
	"CB_OK,                     \n",			// Note: Do not replace spaces with tabs. Strings must have identical size
	"CB_CRC_MISMATCH,           \n",
	"CB_PARITY_MISMATCH,        \n",
	"CB_CRC_AND_PARITY_MISMATCH,\n",
	"CB_DATA_LOSS,              \n",
};

u32 summary_add_str(const char *format, ...) {
	va_list valist;
	va_start(valist, format);
	summary.count += vsprintf(summary.buf + summary.count, format, valist);
	va_end(valist);
	return summary.count;
}
#define SHOULD_BUILD_JSON()    (ctx.verbose_level >= 1)
#define SHOULD_PRINT_VERBOSE() (ctx.verbose_level >= 2)

void __on_error(void) {
	if (!ctx.had_errors) {
		ctx.had_errors = true;
		if (SHOULD_BUILD_JSON()) {
			summary_add_str("{\n\t\"rlba\":%u,\"enum\":", ctx.rlba);
			ctx.sum_buf_ind_of_retval = summary.count;
			summary_add_str("%s",retval_string[0]); // Leave space for total_enum value.
		}
	}
}

#define SUM_RLBA_RV         "{\n\t\"rlba\":%u,\"enum\":%s"
#define SUM_PARITY_MISMATCH "\t\"parity_mismatch\":[{\"role\":%u}],\n"
#define SUM_EDIC_MISMATCH   "\t\"edic_mismatch\":[{\"role\":%u,\"original_edic\":0x%08x,\"calced_edic\":0x%08x}],\n"
#define SUM_BAD_SECTOR      "\t\"bad_sector\":[{\"role\":%u}],\n"

void summary_finish_str(cmp_blocks_retval rv) {
	BUG_ON((rv != CB_OK) ^ (ctx.had_errors));		// Must be identical. If had error, rv cant be ok
	if (SHOULD_PRINT_VERBOSE()) {
		pr_info("Summary: ");
		if (rv == CB_OK)
			pr_info("slice is valid");
		else {
			if (rv & CB_PARITY_MISMATCH)
				pr_info("parity does not match, ");
			if (rv & CB_CRC_MISMATCH)
				pr_info("crc is invalid, ");
			if (rv & CB_DATA_LOSS)
				pr_info("data loss!!!, ");
		}
		pr_info("\n");
	}
	if (ctx.had_errors && SHOULD_BUILD_JSON()) {
		memcpy(summary.buf + ctx.sum_buf_ind_of_retval, retval_string[rv], strlen(retval_string[rv]));
		summary_add_str("},\n"); // Closing of slice obj
	}
}

static char *u322bin(u32 u, char res[33]) {
	u32 i, mask = 1 << 31; // fill in values right-to-left
	res[32] = '\0';
	for (i = 0; i < 32; i++, mask >>= 1)
		res[i] = ((u & mask) != 0) + '0';
	return res;
}
extern u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled);
cmp_blocks_retval cmp_blocks_mirror(cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN], const bool dbg_di, const uint64_t rlba, const int _verbose, const bool has_metadata) {
	cmp_blocks_retval rv = CB_OK;
	int i;
	const enum nvmeibc_data_written_state md_state[2] = { nbdpec_md_get_data_written_state(&f_data[0].md), nbdpec_md_get_data_written_state(&f_data[1].md)};
	const bool unwritten[2] = { nbdpec_md_was_data_never_written(&f_data[0].md), nbdpec_md_was_data_never_written(&f_data[1].md)};
	const bool bad_sector[2] = {(md_state[0] == DATA_EXPLICITLY_MARKED_INVALID), (md_state[1] == DATA_EXPLICITLY_MARKED_INVALID)};
	ctx.rlba = rlba; ctx.verbose_level = _verbose; ctx.had_errors = 0;
	if (has_metadata) {						// Step 1. Check state of blocks
		if (unwritten[0] && unwritten[1]) {	// Slice never written
			if (SHOULD_PRINT_VERBOSE())
				pr_debug("skip never written slice\n");
			goto _out;
		} else if (unwritten[0] != unwritten[1]) {
			rv = CB_PARITY_MISMATCH;	// Corruption, 1 is written the other is not
		} else if (bad_sector[0] && bad_sector[1]) {
			if (SHOULD_PRINT_VERBOSE())
				pr_debug("bad sectors in entire slice. Data is lost\n");
			__on_error();
			rv = CB_DATA_LOSS;
			goto _after_metadata_check;
		} else if (bad_sector[0] != bad_sector[1]) {
			rv = CB_PARITY_MISMATCH;	// Corruption, 1 is bas sector the other is not, can be recovered
		}
	}
	if (rv == CB_OK) {	// Compare data blocks, Both written
		if (data_blk_cmp((const data_blk *)f_data[0].segment, (const data_blk *)f_data[1].segment) < 0)
			rv = CB_PARITY_MISMATCH;
	}

	if (!has_metadata)
		goto _after_metadata_check;
	for (i = 0; i < 2; ++i) {
		union nvmeibc_block_dp_ec_data_block_md *md = &f_data[i].md;
		const bool is_parity = NVMEIBC_DATA_MD_MIRROR_IS_PARITY ? true : (i > 0);
		const u32 edic_calc = nvmeibc_calculate_edic_from_data_and_rlba(rlba, (unsigned char *)f_data[i].segment, dbg_di);
		const u32 edic_md = (u32)(is_parity ? md->P.edic : md->D.edic);
		const u32 crc_mask = NVMEIBC_DP_EC_MD_EDIC_MASK(is_parity);
		const bool edic_ok = (((edic_md ^ edic_calc) & crc_mask) == 0);
		const bool is_special_md = ((bad_sector[i]) || (md_state[i] == DATA_EXPLICITLY_MARKED_NEVERWRITTEN));
		if (SHOULD_PRINT_VERBOSE()) {
			char print_buf[256];
			nbdpec_md_to_string_buf(md, (is_parity ? 'P' : 'D'), print_buf);
			pr_debug("d%d, md: %s\n", i, print_buf);
			if (!is_special_md) {
				if ((md->tx_id != NVMEIBC_MIRROR_UNUSED_TXID_JRI) || (md->jri != NVMEIBC_MIRROR_UNUSED_TXID_JRI))
					pr_debug("     \\-> wrong txid/jri, was metadata written externally?\n");
			}
			md->P.edic = edic_calc;		// Fix edic
		}
		// {FILE *fl_md_out = fopen("./zzzzz.bin", "wb"); fwrite(md, sizeof(md), 1, fl_md_out); fclose(fl_md_out);}

		if (!edic_ok) {
			__on_error();
			if (SHOULD_BUILD_JSON())
				summary_add_str(SUM_EDIC_MISMATCH, i, edic_md & crc_mask, edic_calc & crc_mask);
			rv |= CB_CRC_MISMATCH;
		}
		if (bad_sector[i]) {
			__on_error();
			if (SHOULD_BUILD_JSON())
				summary_add_str(SUM_BAD_SECTOR, i);
		}
	}
 _after_metadata_check:
	if (rv & CB_PARITY_MISMATCH) {
		__on_error();
		if (SHOULD_BUILD_JSON())
			summary_add_str(SUM_PARITY_MISMATCH, 1);
	}
 _out:
	summary_finish_str(rv);
	return rv;
}

cmp_blocks_retval gen_md(cmp_blocks_file_data *f_data, const struct _gen_md_params *p, const bool dbg_di, const uint64_t rlba, const bool fix_md, const int _verbose) {
	char print_buf[256];
	const union nvmeibc_block_dp_ec_data_block_md *md = &f_data->md;
	cmp_blocks_retval rv = CB_OK;
	const u32 crc_mask = NVMEIBC_DP_EC_MD_EDIC_MASK(p->is_parity);
	const char c = (p->is_parity ? 'P' : 'D');
	const u32 edic_calc = nvmeibc_calculate_edic_from_data_and_rlba(rlba, (unsigned char *)f_data->segment, dbg_di);
	const enum nvmeibc_data_written_state md_state = nbdpec_md_get_data_written_state(md);
	const bool unwritten = nbdpec_md_was_data_never_written(md);
	const bool bad_sector = (md_state == DATA_EXPLICITLY_MARKED_INVALID);
	ctx.rlba = rlba; ctx.verbose_level = _verbose; ctx.had_errors = 0;
	if (SHOULD_PRINT_VERBOSE()) {
		nbdpec_md_to_string_buf(md, c, print_buf);
		pr_debug("Reading md: %s\n", print_buf);
	}
	if (!unwritten) {
		const u32 edic_md = (u32)(p->is_parity ? md->P.edic : md->D.edic);
		const bool edic_ok = (((edic_md ^ edic_calc) & crc_mask) == 0);
		if (!edic_ok) {
			nbdpec_md_to_string_buf(md, c, print_buf);
			if (SHOULD_PRINT_VERBOSE())
				pr_debug("Wrong edic, should be=0x%x!\n", edic_calc);
			__on_error();
			if (SHOULD_BUILD_JSON())
				summary_add_str(SUM_EDIC_MISMATCH, 0, edic_md & crc_mask, edic_calc & crc_mask);
			rv |= CB_CRC_MISMATCH;
		}
	}
	if (bad_sector) {
		__on_error();
		if (SHOULD_BUILD_JSON())
			summary_add_str(SUM_BAD_SECTOR, 0);
	}
	(void)fix_md;
	if (p->is_r1)
		nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_r1(&f_data->md, edic_calc);
	else
		nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_ec(&f_data->md, NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS, p->is_parity, edic_calc, false, &p->dbits);
	if (SHOULD_PRINT_VERBOSE()) {
		nbdpec_md_to_string_buf(md, c, print_buf);
		pr_debug("Writing md: %s\n", print_buf);
	}
	summary_finish_str(rv);
	return rv;
}

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#define get_data_u64(d_blk)	 (*d_blk)
#else
	#define get_data_u64(d_blk)	 (d_blk)->dont_touch_original.data
#endif
static void __print_blocks_to_stderr(data_blk *d[2], char *str_out, ssize_t str_out_len, const bool dbg_di, const int i) {
	const bool is_same = !(data_blk_cmp(d[0], d[1]) < 0);
	if (dbg_di) {
		data_blk_to_string(d[1], str_out, str_out_len);
		pr_info("The original   seg[%d]: %s\n", i, str_out);
		data_blk_to_string(d[0], str_out, str_out_len);
		pr_info("The calculated seg[%d]: %s\n", i, str_out);
	} else {
		pr_info("The original   seg[%d]: {0x%016llx}\n", i, get_data_u64(d[1]));
		if (!is_same)
			pr_info("The calculated seg[%d]: {0x%016llx}\n", i, get_data_u64(d[0]));
	}
}

cmp_blocks_retval cmp_blocks_ec(const u32 data_size, const u32 parity_size, cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN], const bool check_crc, const bool dbg_di, __attribute__ ((unused)) const u32 snake_size, const uint64_t rlba, const bool fix_data, const bool fix_crc, const int _verbose, const u32 reconst_bmp)
{
	u32 *crc[N_MAX_RAID_SLICE_LEN];
	u32 crc_data[N_MAX_RAID_SLICE_LEN] = {0};
	u32 i, in_bm, out_bm, crc_bm, mask;
	cmp_blocks_retval rv = CB_OK;
	ssize_t str_out_len = 4*NVMEIBC_SECTOR_SIZE;
	char str_out[str_out_len];
	u64 *segments[N_MAX_RAID_SLICE_LEN];
	void *rawAlloc = NULL;
	const u32 n_segs = data_size + parity_size;
	const u32 n_segs_mask = (1 << n_segs) - 1;
	u32 max_txid = 0;
	bool is_slice_neverwritten = false;
	ctx.rlba = rlba; ctx.verbose_level = _verbose; ctx.had_errors = 0;
	if (data_size == 1 && parity_size == 1) {
		pr_info("Not handling RAID1 only the real cmp_block projects handles it.");
		return CB_RUNTIME_ERROR;
	}

	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i) { //segmetns allocation
		EXIT_ON(posix_memalign(&rawAlloc, 64, NVMEIBC_SECTOR_SIZE));	 // Align to 64 bytes, required by GF asm functions for AVX2, AVX1 needed 32.
		if (!rawAlloc) {
			pr_info("Mem allocation for segments failed\n");
			return CB_RUNTIME_ERROR;
		}
		segments[i] = rawAlloc;
	}

	for (i = 0; i < n_segs; ++i)
		memcpy(segments[i], &f_data[i].segment, NVMEIBC_SECTOR_SIZE);


	//crc address init
	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i)
		crc[i] = &crc_data[i];

	if (reconst_bmp) {										// Reconstruct at data and possibly paritiy
		in_bm = reconst_bmp;
		out_bm = (n_segs_mask & (~reconst_bmp));
		crc_bm = check_crc ? out_bm : 0;
	} else {												// Generate parities from data & all crcs
		in_bm = n_segs_mask;
		out_bm = GENMASK(parity_size - 1, 0) << data_size;
		crc_bm = check_crc ? n_segs_mask : 0;
	}

	if (1) {		// Calculate if slice was never written
		const u32 parity_mask = GENMASK(parity_size - 1, 0) << data_size;
		const unsigned long valid_parity_bm = reconst_bmp ? (unsigned long)(in_bm & parity_mask) : 0UL;
		if (valid_parity_bm) {
			const int pari_index = (int)find_first_bit((void*)&valid_parity_bm, n_segs);
			is_slice_neverwritten = nbdpec_md_was_data_never_written(&f_data[pari_index].md); // If parity was not written, then nothing in slice was written
		} else { // If no valid pari then check datas
			bool at_least_1_data_written = false;
			for (i = 0; i < data_size; i++) {
				at_least_1_data_written |= !nbdpec_md_was_data_never_written(&f_data[i].md);
			}
			is_slice_neverwritten = (!at_least_1_data_written);
		}
	}
	for (i = 0, mask = 1; i < n_segs; ++i, mask <<= 1) {
		if ((mask & in_bm) && (f_data[i].md.tx_id > max_txid))
			max_txid =  f_data[i].md.tx_id;
	}

	if (SHOULD_PRINT_VERBOSE()) {
		char print_buf[256];
		pr_debug("Bmps: in=0x%x, out=0x%x, crc=0x%x, snw=%d, max_txid=0x%x\n", in_bm, out_bm, crc_bm, is_slice_neverwritten, max_txid);
		for (i = 0; i < n_segs; ++i) {
			const char c = ((i >= data_size) ? 'P' : 'D');
			nbdpec_md_to_string_buf(&f_data[i].md, c, print_buf);
			pr_debug("%c[%d], md: %s\n", c, i, print_buf);
		}
	}
	nvmeibc_reed_solomon_fill_missing(n_segs, data_size, snake_size, (void*)segments, NULL, crc, in_bm, out_bm, crc_bm, rlba, dbg_di);

	// compare data and crc
	for (i = 0, mask = 1; i < n_segs; ++i, mask <<= 1) {
		data_blk *d[2] = {(void*)segments[i], (void*)f_data[i].segment};
		const bool do_blocks_differ = (data_blk_cmp(d[0], d[1]) < 0);
		const bool isParity = (i >= data_size);
		const u32 crc_mask = NVMEIBC_DP_EC_MD_EDIC_MASK(isParity);
		if (do_blocks_differ) {
			__on_error();
			if (SHOULD_BUILD_JSON()) summary_add_str(SUM_PARITY_MISMATCH, i);
			if (SHOULD_PRINT_VERBOSE()) {
				pr_info("%s %d is not matching\n", (i >= data_size) ? "parity" : "block", i);
				__print_blocks_to_stderr(d, str_out, str_out_len, dbg_di, i);
			}
			rv |= CB_PARITY_MISMATCH;
		}

		if (1) {		// Compare edics of regenerated and original source data+parities
			const u64 rlba_offset = (isParity ? 0 : i);
			const u32 edic_md =     (u32)(isParity ? f_data[i].md.P.edic : f_data[i].md.D.edic);
			const u32 edic_expect = (mask & crc_bm) ? crc_data[i] : nvmeibc_calculate_edic_from_data_and_rlba(rlba + rlba_offset, (unsigned char *)segments[i], dbg_di);
			const bool found_crc_match = (((edic_expect ^ edic_md) & crc_mask) == 0);
			const bool is_neverwritten_block = nbdpec_md_was_data_never_written(&f_data[i].md);
			if (!found_crc_match && (!is_neverwritten_block)) {
				__on_error();
				if (SHOULD_BUILD_JSON()) summary_add_str(SUM_EDIC_MISMATCH, i, edic_md & crc_mask, edic_expect & crc_mask);
				if (SHOULD_PRINT_VERBOSE()) {
					char binary_edic[33];
					pr_info("crc of seg[%d] wrong!\n", i);
					pr_info("\tOriginal crc is: 0x%08x (%sb)   (first bits might be omitted)\n",  edic_md & crc_mask, u322bin(       edic_md & crc_mask, binary_edic));
					pr_info("\tCorrect  crc is: 0x%08x (%sb)\n",                           edic_expect & crc_mask, u322bin(edic_expect & crc_mask, binary_edic));
					if (1) {	// Check, edic matched the original block before it was reconstructed
						const u32 edic_calc_orig = nvmeibc_calculate_edic_from_data_and_rlba(rlba + rlba_offset, (unsigned char *)f_data[i].segment, dbg_di);
						const bool edic_ok = (((edic_calc_orig ^ edic_md) & crc_mask) == 0);
						if (edic_ok) {
							pr_info("\tNote: Original (metadata) edic did match the original data, but does not match reconstructed data\n");
						}
					}
					if (!do_blocks_differ) {	// Otherwise, we already printed this
						__print_blocks_to_stderr(d, str_out, str_out_len, dbg_di, i);
					}
					pr_info("\n");
				}
				rv |= CB_CRC_MISMATCH;
			}
			crc_data[i] = (edic_expect & crc_mask);	// Thus we have all edics in their final form for D+P blocks in this array
		}

		if (1) {	// Write fixed results to data and metadata
			const bool do_fix_this_block = (fix_data && (mask & out_bm));
			if (do_fix_this_block && do_blocks_differ)
				memcpy(&f_data[i].segment, segments[i], NVMEIBC_SECTOR_SIZE);
			if (fix_crc) {			// Fix metadata
				nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_ec(&f_data[i].md, max_txid, isParity, crc_data[i], is_slice_neverwritten, NULL);
			}
		}
	}	// For: each seg

	// If parity was mismatch and we have 2+ parities, attempt to fix a single error
	if ((rv & CB_PARITY_MISMATCH) && SHOULD_BUILD_JSON() && !reconst_bmp) {
		u32 cand_i, skip_i;
		crc_bm = 0;
		for (skip_i = 0; skip_i < n_segs; ++skip_i) {
			u32 n_matching = 0;
			for (cand_i = 0; cand_i < n_segs; ++cand_i) {
				out_bm = ((1U << cand_i) | (1U << skip_i));
				if (skip_i == cand_i)
					continue;

				for (i = 0; i < n_segs; ++i)
					memcpy(segments[i], &f_data[i].segment, NVMEIBC_SECTOR_SIZE);

				in_bm = ~((1U << cand_i) | (1U << skip_i));
				nvmeibc_reed_solomon_fill_missing(n_segs, data_size, snake_size, (void*)segments, NULL, crc, in_bm, out_bm, crc_bm, rlba, dbg_di);

				//compare data
				if (!data_blk_cmp((void*)segments[cand_i], (void*)f_data[cand_i].segment)) {
					const bool is_skip_matching = (!data_blk_cmp((void*)segments[skip_i], (void*)f_data[skip_i].segment));
					if (is_skip_matching) {
						summary_add_str("\t\"full____slice_match\":[{\"role\":%u,\"skipped_role\":%u}],\n", cand_i, skip_i);
					} else {
						summary_add_str("\t\"partial_slice_match\":[{\"role\":%u,\"skipped_role\":%u}],\n", cand_i, skip_i);
					}
					n_matching++;
					pr_info("skip:%u, cand=%u, wrong_val:0x%llx, fixed_val:0x%llx, cand_val:0x%llx\n", skip_i, cand_i, f_data[skip_i].segment[0], segments[skip_i][0], f_data[cand_i].segment[0]);
				} else {
				}
				if (n_matching == (n_segs - 1)) { // We found that seg 'skip_i' is wrong, and even have its fixed value
					summary_add_str("\t\"reconstruction\":[{\"role\":%u,\"wrong_val\":\"0x%llx\",\"fixed_val\":\"0x%llx\"}],\n", skip_i, f_data[skip_i].segment[0], segments[skip_i][0]);
				}
			}
		}
	}
	if (SHOULD_BUILD_JSON()) {
		if (reconst_bmp) summary_add_str("\t\"reconstruct_bmp\":\"0x%x\",\n", out_bm);
		if (is_slice_neverwritten) summary_add_str("\t\"is_slice_neverwritten\": 1,\n");
	}
	summary_finish_str(rv);
	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i) // segments deallocation
		free(segments[i]);
	return rv;
}
