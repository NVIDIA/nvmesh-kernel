/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "./cmp_blocks_impl.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_defs.h"
#include "kr_incs.h"
#include "../../../common/nvmeib_math.h"
#include <errno.h>

#define MD_SIZE sizeof(union nvmeibc_block_dp_ec_data_block_md)		// == 8

cmp_blocks_retval check_mirror(char fn[][PATH_MAX], bool dbg_di);
#define VERSION "1.9" /* Please make sure to update this in case breaking changes are made */
#define COL_GR "\x1b[32m" // green
#define COL_RED "\x1b[1;31m" // Bold red
#define COL_R "\x1b[0;0m" // Reset color
void print_usage(void) {
	fprintf(stderr, "USAGE: ./cmp_blocks --verbose <true/false> --dbg_di <true/false> -d <number> -p <number> --d_path <data_prefix>_%%d --md_path <md_prefix>_%%d [-s <snake size>  Default 1] [--rlba <rlba number of the owner block of first slice or of the slice in slice_index>] (OPTIONAL if omitted no crc will be checked) [--slice_ind <slice index> slice index from start of blockset, exist iff QLC, by default 0 ] [--reconst_bmp bitmap in hex representation string of the valid datas which to restore from - optional if not present then a restoration will not be performed]\nThe file names starting from 0.\n");
	fprintf(stderr, "Example: ./cmp_blocks --verbose true --dbg_di true -d 8 -p 2 --d_path data_%%d --md_path metadata_%%d\n");
	fprintf(stderr, "Bash cmd to manually analyze: for i in {0..9}; do ./parse_block data_${i} data_${i}.txt; done; cat data_*.txt > all.txt;\n");
	fprintf(stderr, "For QLC there should be only one md file with <md_prefix>_0, and --slice_ind must be given\n");
	fprintf(stderr, COL_GR "---------------------- R1 ----------------------\n" COL_R);
	fprintf(stderr, "* Example: ./cmp_blocks examples/R1_no_md/data_0 examples/R1_no_md/data_1\n");
	fprintf(stderr, "     * Backward Compatible with Old V1.2.1 api of cmp_blocks.c usage for R1: ./cmp_blocks <binary_block_file1> <binary_block_file2>\n");
	fprintf(stderr, "* ./cmp_blocks --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_no_md/data_%%d --md_path NULL\n");
	fprintf(stderr, "* ./cmp_blocks --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_no_md/corrupt_%%d\n");
}

struct params {
	uint64_t first_rlba;
	u32  data_size;			// D + P
	u32  parity_size;
	u32  snake_size;
	bool check_crc;
	bool dbg_di;
	bool verbose;
	bool dry_run;
	u32 reconst_bmp;
	struct _gen_md_params gen_md;	// Gen_md param only
	bool is_compatible;		// Old raid1 compatibility mode
	bool is_mirror;			// Working with raid1 blocks, with appropriate metadata format
	bool is_gen_md_mode;
	bool do_fix;			// Actually output fixed data/metadata to disk
} par = {	.first_rlba = ~0ULL,
			.data_size = 1, .parity_size = 1, .snake_size = 1,
			.check_crc = false, .dbg_di = false, .verbose = false, .dry_run = true,
			.reconst_bmp = 0,		// No reconstruction
			.gen_md = {.dbits = {.all_bits = 0}, .is_parity = false, .is_r1 = false},
			.is_compatible = false,
			.is_mirror = false, .is_gen_md_mode = false, .do_fix = false};		// Default values

static int __parse_args(int argc, const char *argv[], struct params *p, const char **d_fn_template, const char **md_fn_template) {
	int i, rv = 0;
	if (0) print_args(argc, argv);
	if (argc < 3) {						// help
		print_usage();
		return -1;
	}
	p->is_compatible = (argc == 3);		// miror compatability mode
	if (p->is_compatible) {
		p->is_mirror = true;
		return 0;
	}
	if ((argc < 13) || ((argc % 2) == 0)) {	// 13 mandatory + optional pairs
		stop_on_error("Error: Wrong amount of arguments: %d\n", argc);
	}
	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--verbose")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->verbose = (c == 't');
		} else if (!strcmp(argv[i], "--dbg_di")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->dbg_di = (c == 't');
		} else if (!strcmp(argv[i], "-d")) {
			p->data_size = (u32)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-p")) {
			p->parity_size = (u32)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--d_path")) {
			*d_fn_template = argv[++i];
		} else if (!strcmp(argv[i], "--md_path")) {
			*md_fn_template = argv[++i];
		} else if (!strcmp(argv[i], "-s")) { // Snake size != 1
			p->snake_size = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--rlba")) { //crc need to be checked
			p->first_rlba = strtoll(argv[++i], NULL, 10);
			p->check_crc = true;
		} else if (!strcmp(argv[i], "--reconst_bmp")) {
			if (!(p->reconst_bmp = (u32)strtol(argv[++i], NULL, 16)))
				stop_on_error("Error: Reconstruct bitmap=0x%x cannot be zero, it can be ommited\n", p->reconst_bmp);
		} else if (!strcmp(argv[i], "--dry_run")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->dry_run = (c == 't');
		} else if (!strcmp(argv[i], "--role")) {
			const char c = argv[++i][0];
			if ((c != 'D')&&(c != 'P')&&(c != 'R'))
				stop_on_error("%dth argument is neither D/P/R1, %s\n", i, argv[i]);
			p->gen_md.is_r1 = (c == 'R');
			p->gen_md.is_parity = (c == 'P') || (p->gen_md.is_r1 && NVMEIBC_DATA_MD_MIRROR_IS_PARITY);
			p->is_gen_md_mode = true;
		} else if (!strcmp(argv[i], "--dbits")) {
			const char *dbits_fmt = "{%u/%u}";
			int dbit0 = -1, dbit1 = -1;
			if (2 != sscanf(argv[++i], dbits_fmt, &dbit0, &dbit1))
				stop_on_error("dbits value must have format %s, (-1 for none, x for seg x), not %s\n", dbits_fmt, argv[i]);
			p->gen_md.dbits = nvmeib_dbits_entry_build_for_segs(dbit0, dbit1);
		} else {
			stop_on_error("Error: Invalid %dth argumet: %s\n", i, argv[i]);
		}
	}

	if ((p->data_size + p->parity_size) > N_MAX_RAID_SLICE_LEN)
		stop_on_error("Slice size too big %d+%d > %d\n", p->data_size, p->parity_size, N_MAX_RAID_SLICE_LEN);
	if (p->parity_size > 2)
		stop_on_error("Too much parities %d > 2\n", p->parity_size);
	if (p->snake_size == 0)
		stop_on_error("Zero snake size\n");
	p->is_mirror = (p->data_size == 1);
	p->do_fix = (((p->reconst_bmp != 0) || p->is_gen_md_mode) && !p->dry_run);

	if (p->reconst_bmp) {
		const u32 n_bits = (u32)hweight32(p->reconst_bmp);
		const u32 n_segs_mask = (1 << (p->data_size + p->parity_size)) - 1;
		if (!p->check_crc)
			stop_on_error("Error for reconstruction of blocks need to know the rlba\n");
		if (n_bits < p->data_size)
			stop_on_error("Wrong reconst_bmp=0x%x, not enough valid sources %d < %d!\n", p->reconst_bmp, n_bits, p->data_size);
		if (n_segs_mask <= p->reconst_bmp)
			stop_on_error("Wrong reconst_bmp=0x%x, Must be < 0x%x!\n", p->reconst_bmp, n_segs_mask);
		if (p->is_mirror)
			stop_on_error("Reconstruction is not supported for Raid-1, just copy one leg to another. Missing edic should be regened with 1 column mode!\n");
	}
_out:
	return rv;
}

static void __close_files(FILE *fl[N_MAX_RAID_SLICE_LEN], u32 n_files) {
	u32 i;
	for (i=0; i < n_files; ++i) {
		if (fl[i]) {
			fclose(fl[i]);
			fl[i] = NULL;
		}
	}
}

// returns file sizes read or -1 on error
static int __open_files(FILE *fl[N_MAX_RAID_SLICE_LEN], const char *fn_template, u32 n_files) {
	u32 i;
	int rv = 0;
	long unsigned file_size = 0;
	long unsigned cur_file_size = 0;
	char fn[PATH_MAX];

	for (i=0; i < n_files; ++i) {
		sprintf(fn, fn_template, i);
		fl[i] = fopen(fn, "rb+");
		if (!fl[i])
			stop_on_error("Unable to open file: %s, Errno: %s\n", fn, strerror(errno));
		fseek(fl[i], 0L, SEEK_END);
		cur_file_size = ftell(fl[i]);
		if (file_size && file_size != cur_file_size)
			stop_on_error("Error: File %s has different size from others, file size: %lu[bytes], others: %lu[bytes]\n", fn, cur_file_size, file_size);
		file_size = cur_file_size;
		fseek(fl[i], 0L, SEEK_SET);
	}
	return file_size;
_out:
	return rv;
}

extern u32 summary_add_str(const char *format, ...);
extern struct cmp_blks_archive summary;
extern const char *retval_string[];

int main(int argc, const char *argv[]) {
	bool has_md = true;
	const char *d_fn_template = NULL, *md_fn_template = NULL;
	char fn[PATH_MAX];
	FILE *fl_d[N_MAX_RAID_SLICE_LEN] = {0};
	FILE *fl_md[N_MAX_RAID_SLICE_LEN] = {0};
	cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN];
	u32 i,j, mask, n_segs = 0, n_data_files = 0;
	long long file_size = 0;
	long unsigned nslices = 0;
	cmp_blocks_retval rv = CB_OK;
	u32 sum_buf_ind_of_retval = 0;

	fprintf(stderr, "Version: " VERSION "\n");
	if (util_initialize_gf_layer() != 0) {
		stop_on_error("Could not allocate xsave bufs!\n");
	}
	if (__parse_args(argc, argv, &par, &d_fn_template, &md_fn_template))
		goto _out;
	n_segs = par.data_size + par.parity_size;

	if (par.is_compatible) { // We are in mirror comparison mode.
		char file_name[2][PATH_MAX];
		strcpy(file_name[0], argv[1]);
		strcpy(file_name[1], argv[2]);
		return check_mirror(file_name, par.dbg_di);
	}
	has_md = (md_fn_template && strcmp(md_fn_template, "NULL"));

	if (1) {		// Open data files
		n_data_files = ((par.is_gen_md_mode) ? 1 : n_segs);
		file_size = __open_files(fl_d, d_fn_template, n_data_files);
		if (file_size < 0) {
			goto _out;
		} else if (file_size % NVMEIBC_SECTOR_SIZE != 0)
			stop_on_error("Error data file sizes: %lu[bytes] not divided by sector_size: %u[bytes]\n", (long unsigned)file_size, NVMEIBC_SECTOR_SIZE);
		nslices = file_size / NVMEIBC_SECTOR_SIZE;
	}
	if (has_md) {		// Open meta-data files
		const int n_md_files = (par.is_gen_md_mode ? 1 : n_segs);
		file_size = __open_files(fl_md, md_fn_template, n_md_files);
		if (file_size < 0) {
			goto _out;
		} else if (file_size % MD_SIZE != 0) {
			stop_on_error("Error md file sizes: %lu[bytes] not divided by md size: %lu[bytes]\n", (long unsigned)file_size, MD_SIZE);
		} else if (nslices != file_size/MD_SIZE) {
			stop_on_error("Error md num of num of slices: %lu different from data num of slices: %lu\n", (long unsigned)(file_size/MD_SIZE), nslices);
		}
	}

	summary.len = nslices * 10000;
	summary.buf = (char *)malloc(summary.len); // Each printing of slice is less than 1000 aprox. LKJ: change to malloc

	summary_add_str("***summary***\n{\n");
	sum_buf_ind_of_retval = summary_add_str("\"total_enum\":");
	summary_add_str("%s",retval_string[0]); // Leave space for total_enum value.
	summary_add_str("\"slices\":[\n");

	for (i=0; i < nslices; ++i) {
		const uint64_t rlba = par.first_rlba + par.data_size * i;
		// fprintf(stderr, "Slice %d, rlba=0x%lx=%ld,\n", i, rlba, rlba);

		for (j=0; j < n_data_files; ++j) {	// Read slice data and md
			if (fread(&f_data[j].segment, NVMEIBC_SECTOR_SIZE, 1, fl_d[j]) != 1) { //read slice data
				sprintf(fn, d_fn_template, j);
				stop_on_error("Error reading file: %s\n", fn);
			}

			if (has_md) {//read slice md
					if (fread(&f_data[j].md, MD_SIZE, 1, fl_md[j]) != 1) {
						sprintf(fn, md_fn_template, j);
						stop_on_error("Error reading file: %s\n", fn);
					}
			}
		}

		// check slice validity
		if (par.is_gen_md_mode) {
			// fprintf(stderr, "Params: gen_md {%d+%d}, dbg_di=%d, rlba=%ld, do_fix=0x%x\n", par.data_size, par.parity_size, par.dbg_di, rlba, par.do_fix);
			rv |= gen_md(                                       f_data, &par.gen_md ,  par.dbg_di,                 rlba, par.do_fix,             (par.verbose ? 2 : 1));
		} else if (par.is_mirror) {
			rv |= cmp_blocks_mirror(                            f_data,                par.dbg_di,                 rlba,                         (par.verbose ? 2 : 1), has_md);
		} else {
			fprintf(stderr, "Params: EC {%d+%d}, check_crc=%d, dbg_di=%d, snake=%d, rlba=%ld, reconstruct_bmp=0x%x\n", par.data_size, par.parity_size, par.check_crc, par.dbg_di, par.snake_size, rlba, par.reconst_bmp);
			rv |= cmp_blocks_ec(par.data_size, par.parity_size, f_data, par.check_crc, par.dbg_di, par.snake_size, rlba, par.do_fix, par.do_fix, (par.verbose ? 2 : 1), par.reconst_bmp);
		}
		if (rv == CB_RUNTIME_ERROR)
			stop_on_error("Got run time error\n");

		if (par.do_fix && par.is_gen_md_mode ) {
			fseek(fl_md[0], -MD_SIZE, SEEK_CUR);
			if (fwrite(&f_data[0].md, MD_SIZE, 1, fl_md[0]) != 1) {
				sprintf(fn, md_fn_template, j);
				stop_on_error("Error writing to file: %s\n", fn);
			}
		} else if (par.do_fix) {
			const u32 invalid_bmp = ~par.reconst_bmp;
			for (j = 0, mask = 1; j < n_segs; ++j, mask <<= 1) {
				if (invalid_bmp & mask) {
					fseek(fl_d[j], -NVMEIBC_SECTOR_SIZE, SEEK_CUR);
					if(fwrite(&f_data[j].segment, NVMEIBC_SECTOR_SIZE, 1, fl_d[j]) != 1) {
						sprintf(fn, d_fn_template, j);
						stop_on_error("Error writing to file: %s\n", fn);
					}

					fseek(fl_md[j], -MD_SIZE, SEEK_CUR);
					if (fwrite(&f_data[j].md, MD_SIZE, 1, fl_md[j]) != 1) {
						sprintf(fn, md_fn_template, j);
						stop_on_error("Error writing to file: %s\n", fn);
					}
				}
			}
		}
	}

	memcpy(summary.buf + sum_buf_ind_of_retval, retval_string[rv], strlen(retval_string[rv]));
	summary_add_str("]\n"); // Closing of slices
	summary_add_str("}\n"); // Closing of object
	summary_add_str("***summary - end***\n");
	memcpy(summary.buf + summary.count, "", 1); // put null terminating char at the end.
	puts(summary.buf);
	if (summary.buf)
		free(summary.buf);
	__close_files(fl_d, n_segs);
	if (!has_md)
		__close_files(fl_md, n_segs);
	return rv;

_out:
	if (summary.buf)
		free(summary.buf);
	__close_files(fl_d, n_segs);
	if (!has_md)
		__close_files(fl_md, n_segs);
	return CB_RUNTIME_ERROR;
}

cmp_blocks_retval check_mirror(char fn[][PATH_MAX], bool dbg_di) {
	#define BLOCK_SIZE (4096)
	ssize_t str_out_len = 4*BLOCK_SIZE;
	char b0[BLOCK_SIZE], b1[BLOCK_SIZE], str_out[str_out_len];
	data_blk *d[2] = {(void*)b0, (void*)b1};
	long unsigned sizes[2], blk_i = 0;
	int i, rv = -1;
	FILE *fl[2] = {NULL, NULL};

	for (i = 0; i < 2; i++) {
		fl[i] = fopen(fn[i], "rb");
		if (!fl[i])
			goto _out;
	}
	for (i = 0; i < 2; i++) {
		fseek(fl[i], 0L, SEEK_END);
		sizes[i] = ftell(fl[i]);
		fseek(fl[i], 0L, SEEK_SET);
		fprintf(stderr, "File %s size: %lu[bytes]=%lu[blks]\n", fn[i], sizes[i], sizes[i]/BLOCK_SIZE);
		if ((i>0)&&(sizes[i] != sizes[i-1])) {
			fprintf(stderr, "Error: Files have different sizes\n");
			rv = CB_RUNTIME_ERROR;
			goto _out;
		}
	}

	for (i = 0; i < 2; i++)
		EXIT_ON(fread(d[i], sizeof(b0), 1, fl[i]) != 1);
	for (blk_i = 0; !feof(fl[0]); blk_i++) {
		const int cmp_rv = data_blk_cmp(d[0], d[1]);
		if (cmp_rv < 0) {
			if (dbg_di) {
				fprintf(stderr, "Block: %lu is not matching, rv=%d\n", blk_i, cmp_rv);
				fprintf(stderr, "Injection=%d[b], parsing blocks...\n", data_blk_get_injection_size());
				for (i = 0; i < 2; i++) {
					data_blk_to_string(d[i], str_out, str_out_len);
					fprintf(stderr, "#################### File:%s\n%s", fn[i], str_out);
				}
			}
			rv = CB_PARITY_MISMATCH;
			goto _out;
		}
		for (i = 0; i < 2; i++)
			EXIT_ON(fread(d[i], sizeof(b0), 1, fl[i]) > 1);
	}
	rv = CB_OK;
_out:
	if (rv == CB_OK)
		fprintf(stderr, COL_GR "All %lu blocks are identical" COL_R "\n", blk_i);
	else
		fprintf(stderr, COL_RED "Summary: Not Identical! First non identical block is %lu!" COL_R "\n", blk_i);

	for (i = 0; i < 2; i++) {
		if (!fl[i])
			fprintf(stderr, "could not open file: %s\n", fn[i]);
		else
			fclose(fl[i]);
	}
	return rv;
}
