#include "common/kr_incs.h"
#include "nvmeib_shared.h"
#include <errno.h>
#include "json.h"
#include <unistd.h>
#include "nvmeib_types.h"
#include "../../../clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"






#define VERSION "1.9" /* Please make sure to update this in case breaking changes are made */
#if defined(pr_debug)
	#undef pr_debug
#endif
#define pr_debug(fmt, ...)  ({ if (par.verbose) fprintf(stderr, fmt, ##__VA_ARGS__); else {}; })

void print_usage(void) {
	fprintf(stderr, "Version: " VERSION " --- Deprecated, use cmp_blocks instead!\n");
	fprintf(stderr, "Used to generate edic and metadata for 4KB blocks.\n");
	fprintf(stderr, "USAGE: ./gen_md --dbg_di <true/false> --d_path <data_path> --md_path <md_path> --rlba <rlba of block>\n");
	fprintf(stderr, "* Example: ./gen_md --verbose true --dbg_di false --rlba 227 --d_path ./examples/data4k --md_path ./examples/md_wrong_edic\n");
	fprintf(stderr, "* Example: ./gen_md --verbose true --dbg_di false --rlba 227 --d_path ./examples/data4k --md_path ./examples/md_never_written\n");
}
struct params {
	uint64_t rlba;
	const char *d_fn_template;
	const char *md_fn_template;
	union nvmeibc_dbits_entry dbits;
	bool dbg_di;
	bool verbose;
	bool is_parity;
	bool is_r1;			// Treat the block as comming from mirrored volume with appropriate metadata format
	bool dry_run;
} par = {.rlba = ~0ULL, .d_fn_template = NULL, .md_fn_template = NULL, .dbits = {.all_bits = 0}, .dbg_di = false, .verbose = false, .is_parity = false, .dry_run = false};

static void __print_params(const struct params *p) {
	if (p->verbose) {
		char buf[64];
		nvmeibc_dbits_entry_to_str(buf, sizeof(buf), (long)p->dbits.all_bits);
		pr_debug("***** Params: rlba=0x%lx=%ld, dbg_di=%d, is_parity=%d, is_r1=%d, dry_run=%d, dbits=%s\n", par.rlba, par.rlba, par.dbg_di, par.is_parity, par.is_r1, par.dry_run, buf);
	}
}

static int __parse_args(const int argc, const char *argv[], struct params *p) {
	int i, rv = 0;
	if (0) print_args(argc, argv);
	if ((argc < 7) || ((argc % 2) == 0))	// 7 mandatory + optional pairs
		stop_on_error("Error: Wrong amount of arguments: %d\n", argc);
	for (i = 1; i < argc; ++ i) {
		if (!strcmp(argv[i], "--dbg_di")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->dbg_di = (c == 't');
		} else if (!strcmp(argv[i], "--verbose")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->verbose = (c == 't');
		} else if (!strcmp(argv[i], "--role")) {
			const char c = argv[++i][0];
			if ((c != 'D')&&(c != 'P')&&(c != 'R'))
				stop_on_error("%dth argument is neither D/P/R1, %s\n", i, argv[i]);
			p->is_r1 = (c == 'R');
			p->is_parity = (c == 'P') || (p->is_r1 && NVMEIBC_DATA_MD_MIRROR_IS_PARITY);
		} else if (!strcmp(argv[i], "--d_path")) {
			p->d_fn_template = argv[++i];
		} else if (!strcmp(argv[i], "--md_path")) {
			p->md_fn_template = argv[++i];
		} else if (!strcmp(argv[i], "--rlba")) {
			p->rlba = strtoll(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--dry_run")) {
			const char c = argv[++i][0];
			if ((c != 't')&&(c != 'f'))
				stop_on_error("%dth argument is neither true/false, %s\n", i, argv[i]);
			p->dry_run = (c == 't');
		} else if (!strcmp(argv[i], "--dbits")) {
			const char *dbits_fmt = "{%u/%u}";
			int dbit0 = -1, dbit1 = -1;
			if (2 != sscanf(argv[++i], dbits_fmt, &dbit0, &dbit1))
				stop_on_error("dbits value must have format %s, (-1 for none, x for seg x), not %s\n", dbits_fmt, argv[i]);
			p->dbits = nvmeib_dbits_entry_build_for_segs(dbit0, dbit1);
		} else {
			stop_on_error("Error: Invalid %dth argumet: %s\n", i, argv[i]);
		}
	}
	return 0;
_out:
	print_usage();
	return rv;
}

int main(int argc, const char *argv[]) {
	char md_in[256], md_out[256], md_ouj[256], print_buf[256];
	FILE *fl_d = NULL, *fl_md_in = NULL, *fl_md_out = NULL, *fl_md_ouj = NULL;
	u64 data[NVMEIBC_SECTOR_SIZE/8], data_file_size;
	int rv = 0, n_blocks = 0;
	struct json_object *root = NULL, *edic_json = NULL;

	fprintf(stderr, "Version: " VERSION "\n");
	util_initialize_gf_layer();
	if ((rv = __parse_args(argc, argv, &par)))
		goto _out;
	__print_params(&par);

	// Open files
	snprintf(md_in,  sizeof(md_in),  "%s.in"  , par.md_fn_template);
	snprintf(md_out, sizeof(md_out), "%s.out" , par.md_fn_template);
	snprintf(md_ouj, sizeof(md_ouj), "%s.json", par.md_fn_template);
	fl_d = fopen(par.d_fn_template, "rb"); if (!fl_d) stop_on_error("Unable to open file: %s, Errno: %s\n", par.d_fn_template, strerror(errno));
	fl_md_in  = fopen(md_in,  "rb");   if (!fl_md_in)  stop_on_error("Unable to open file: %s, Errno: %s\n", md_in, strerror(errno));
	if (!par.dry_run) {
		fl_md_out = fopen(md_out, "wb");   if (!fl_md_out) stop_on_error("Unable to open file: %s, Errno: %s\n", md_out, strerror(errno));
		// fl_md_ouj = fopen(md_ouj, "wt");   if (!fl_md_ouj) stop_on_error("Unable to open file: %s, Errno: %s\n", md_ouj, strerror(errno));
		if ((!(root = json_create_object())) || (!(edic_json = json_create_object()))) {
			stop_on_error("Cant create Json");
		}
	}

	fseek(fl_d, 0L, SEEK_END);
	data_file_size = ftell(fl_d);
	fseek(fl_d, 0L, SEEK_SET);
	n_blocks = data_file_size / sizeof(data);
	(void)n_blocks;

	rv = fread(data, sizeof(data), 1, fl_d);
	if (rv != 1)
		stop_on_error("Unable to read file: %s, Errno: %s\n", par.d_fn_template, strerror(errno));
	pr_debug("Read 1 block from %s: %d[b], from %u[blocks]\n", par.d_fn_template, (int)(rv * sizeof(data)), n_blocks);
	rv = 0;
	{	// Process currrent slice
		const u32 crc_mask = NVMEIBC_DP_EC_MD_EDIC_MASK(par.is_parity);
		union nvmeibc_block_dp_ec_data_block_md md;
		const u32 edic = nvmeibc_calculate_edic_from_data_and_rlba(par.rlba, (unsigned char *)data, par.dbg_di);
		const char c = (par.is_parity ? 'P' : 'D');
		size_t	n_read;

		n_read = fread(&md, sizeof(md), 1, fl_md_in);
		if (par.verbose) {
			nbdpec_md_to_string_buf(&md, c, print_buf);
			pr_debug("***** Reading md: %s n_read=%jd\n", print_buf, n_read);
		}
		if (!nbdpec_md_was_data_never_written(&md)) {
			const bool is_edic_correct = (((md.P.edic ^ edic) & crc_mask) == 0);
			if (!is_edic_correct) {
				nbdpec_md_to_string_buf(&md, c, print_buf);
				pr_debug("***** Wrong edic, should be=0x%x!\n", edic);
			}
		}
		if (par.is_r1)
			nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_r1(&md, edic);
		else
			nvmeibc_block_dp_ec_md_set_externally_fixed_by_nvck_ec(&md, NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS, par.is_parity, edic, false, &par.dbits);
		if (par.verbose) {
			nbdpec_md_to_string_buf(&md, c, print_buf);
			pr_debug("***** Writing md: %s\n", print_buf);
		}
		if (!par.dry_run) {
			fwrite(&md, sizeof(md), 1, fl_md_out);
			if ((!(fl_md_ouj = freopen(md_ouj, "w", stdout)))) { stop_on_error("Unable to open file: %s, Errno: %s\n", md_ouj, strerror(errno));}
			json_object_add_value_uint(edic_json, "rlba", par.rlba);
			json_object_add_value_uint(edic_json, "edic", edic);
			json_object_add_value_object(root, "md", edic_json);
			json_print_object(root, NULL);
		}
	}
_out:
	if (fl_d)      fclose(fl_d);
	if (fl_md_in)  fclose(fl_md_in);
	if (fl_md_out) fclose(fl_md_out);
	if (fl_md_ouj) fclose(fl_md_ouj);
	return rv;
}

