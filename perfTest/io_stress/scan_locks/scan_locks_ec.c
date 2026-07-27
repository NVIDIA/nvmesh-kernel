#include "common/kr_incs.h" /*Must be first*/
#include "nvmeib_shared.h"
#include <sys/mman.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <string.h>

// Colors description is here https://telepathy.freedesktop.org/doc/telepathy-glib/telepathy-glib-debug-ansi.html
#define KERN_COL_RESET      "\x1b[0;0m"
#define KERN_COL_RED_BOLD    "\x1b[1;31m"	// Bold format is 1; 31m - RED, 33m is Yellow
#define KERN_COL_RED_BL_BOLD "\x1b[5;31m"	// Blink bold is 5
#define print_error_wrong_arg(...)      printf("Error: " __VA_ARGS__);
#define handle_error(         msg) do { perror("Error: " msg);         exit(EXIT_FAILURE); } while (0)
#define abort_with_msg(       ...) do { printf(KERN_COL_RED_BOLD "Error: " KERN_COL_RESET __VA_ARGS__); exit(EXIT_FAILURE); } while (0)

#define DRV_BLK_SIZE			(4096)
#define NUM_BLKS_IN_BLKSET		(32)

static inline union nvmeib_lock_blkset_entry get_lock_default_reset_val(void) {
	union nvmeib_lock_blkset_entry e;		// Unlock the lock, set TxID to uninitialize, dont touch dbits
	e.all = 0;
	e.blkset_info.bits.txid = 1; /*NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS*/
	return e;
}

/************************** Filter on area on disk ****************************/
#define N_SEGS_ON_DISK (10)		// Support at most this amount of ranges on each disk
#define N_FILTER_DISKS (10)		// Support at most this amount of disks
struct t_filter_disk {
	char name[64];
	struct t_range {
		u64 start, end;			// Scan only [start..end) range
	} range[N_SEGS_ON_DISK];
	u32 n_ranges;				// Number of disk ranges in the array above. 0 - return all info, otherwise, only info within filtered areas
	u32 cur_range;				// Iterator over ranges (as scann of locks proceed, we skip all entries that are not within any range
};

struct t_filter_all {
	struct t_filter_disk disks[N_FILTER_DISKS];
	u32 n_disks;				// Number of active disk filters in the array above. 0 - return all info, otherwise, only info within filtered areas
	u32 cur_disk;				// Index into array above
	struct t_iterator {
		s64 ind;				// 0..N - index of blockset on disk, -X means iterator terminated
	} itr;
};

const char *filters_example = "-filter=2{S3HCNX0JC01929.1=2:1-3/0x1208b-0x120af}{disk2=3:7-9/10-11/30-31}*   #{<disk_name>=<num_ranges>:<start_range1[lock]>-<(start+len)_range1[lock]>/<start_range2[lock]>-<(start+len)_range2[lock]>/....}";
const char* filter_all_unique_stale_locks_example =
		"scan_locks_ec -verbose | grep -e \"### Disk\" -e \"lock-id\" | awk '{ if ($2 ==\"Disk\") { diskName = $3; print $0;} else { if (!seen[diskName\"-\"$4]++){print substr($4, 1, length($4)-1)}}}'";
#define __auto_detect_base(txt) (((txt[0] == '0')&&(txt[1] == 'x')) ? 16 : 10)
static void disk_filters_init(struct t_filter_all *f, char* txt) {
	#define FMT_FILTERS_HEADER "-filter="
	char *orig_str = txt, *end_str = strchr(txt,'*');

	txt += strlen(FMT_FILTERS_HEADER);
	f->n_disks = (u32)strtol(txt, &txt, 10);
	if ((f->n_disks > N_FILTER_DISKS)||(f->n_disks == 0)) {
		print_error_wrong_arg("wrong num disks in filter=%u\n", f->n_disks);
		goto _error;
	}
	// Parse disks
	for (f->cur_disk = 0; f->cur_disk < f->n_disks; f->cur_disk++) {
		struct t_filter_disk *fd = &f->disks[f->cur_disk];
		char *disk_name_end;
		txt += strlen("{");

		if ((disk_name_end = strchr(txt,'='))==NULL)	// Read disk name
			goto _error;
		memcpy(fd->name, txt, (disk_name_end-txt));
		txt = disk_name_end + 1;

		fd->n_ranges = (u32)strtol(txt, &txt, 10);		// Read num ranges
		if ((fd->n_ranges > N_SEGS_ON_DISK)||(fd->n_ranges == 0)||(txt > end_str)) {
			print_error_wrong_arg("wrong num ranges in filter=%u\n", fd->n_ranges);
			goto _error;
		}

		// Parse ranges
		for (fd->cur_range = 0; fd->cur_range < fd->n_ranges; fd->cur_range++) {
			struct t_range *fr = &fd->range[fd->cur_range];
			int base;
			txt++; /* Separator */
			base = __auto_detect_base(txt);
			fr->start = (u64)strtol(txt, &txt, base);
			txt++; /* Separator */
			base = __auto_detect_base(txt);
			fr->end = (u64)strtol(txt, &txt, base);
			if ((fr->end == 0ULL) || (fr->end <= fr->start) || (txt > end_str)) {
				print_error_wrong_arg("Illegal range %u on disk %s [%llu-%llu)\n", fd->cur_range, fd->name, fr->start, fr->end);
				goto _error;
			}
			if ((fd->cur_range > 0) && (fr->start < fr[-1].end)) {
				print_error_wrong_arg("Range %u on disk %s intersects with previous range\n", fd->cur_range, fd->name);
				goto _error;
			}
		}
		txt += strlen("}");
	}
	return;
_error:
	if (txt > end_str)
		txt = orig_str;
	abort_with_msg("Wrong filter format at: ^%s. Example: %s\n", txt, filters_example);
}

static void disk_filters_print(struct t_filter_all *f) {
	for (f->cur_disk = 0; f->cur_disk < f->n_disks; f->cur_disk++) {
		struct t_filter_disk *fd = &f->disks[f->cur_disk];
		printf("-Filter: %02d)%s:", f->cur_disk, fd->name);
		for (fd->cur_range = 0; fd->cur_range < fd->n_ranges; fd->cur_range++) {
			struct t_range *fr = &fd->range[fd->cur_range];
			printf("[0x%llx-0x%llx),", fr->start, fr->end);
		}
		printf("\n");
	}
}

static inline u32 disk_filter_is_active(const struct t_filter_all *f) {
	return (f->n_disks != 0);
}

static u32 disk_filters_should_skip_disk(struct t_filter_all *f, const char *disk_name) {
	if (!disk_filter_is_active(f))
		return 0 /* false*/;	// No filtering activated
	for (f->cur_disk = 0; f->cur_disk < f->n_disks; f->cur_disk++) {
		struct t_filter_disk *fd = &f->disks[f->cur_disk];
		if (!strcmp(fd->name, disk_name))
			return 0 /* false*/;// Scanning the disk is permitted
	}
	return 1 /*true*/;			// Disk not found in permitted list, skip it
}

#define ITERATOR_TERMINATED (-10000ULL)
static u64 filter_rng_start(struct t_filter_all *f, const u64 disk_n_blksets) {
	struct t_filter_disk *fd = &f->disks[f->cur_disk];
	const u64 last_blockset = fd->range[(fd->n_ranges-1)].end - 1;
	fd->cur_range = 0;	// ->cur_disk, ->cur_range correctly set
	f->itr.ind = (s64)fd->range[fd->cur_range].start;
	if (last_blockset >= disk_n_blksets) {
		print_error_wrong_arg("disk %s has only 0x%llx blocksets, filter is out of bound 0x%llx\n", fd->name, disk_n_blksets, last_blockset);
		f->itr.ind = ITERATOR_TERMINATED;
	}
	return (u64)f->itr.ind;
}

static u64 filter_rng_inc(struct t_filter_all *f) {
	struct t_filter_disk *fd = &f->disks[f->cur_disk];
	struct t_range       *fr = &fd->range[fd->cur_range];
	f->itr.ind++;
	if (f->itr.ind >= (s64)fr->end) {
		if (fd->cur_range < (fd->n_ranges-1)) {		// Has next range
			f->itr.ind = fd->range[++fd->cur_range].start;
		} else {
			f->itr.ind = ITERATOR_TERMINATED;
		}
	} // else, staying within current range, do nothing
	return (u64)f->itr.ind;
}

static u32 filter_rng_has_next(struct t_filter_all *f) {
	return (f->itr.ind >= 0);
}

/*********************************** Conf *******************************/
struct t_conf {					// Filter blocksets by configuration
	// Params:
	u32 D      : 8;				// D+P praid
	u32 P      : 8;
	u32 role   : 8;				// Role of the current segment
	u32 stride : 8;				// Default to 2, rotation each 2 blocksets
	u32 n_segs : 8;
	// Calculated values
	u32 n_blksets_full_roll_change : 8;
	u32 dlba_offset : 8;
	u32 is_active : 8;			// Is filtering by configuration is active
	u32 ignore_unused : 8;	// The places in RAM where locks do not reside - should verify that values remain zero?
	u8 lut[32];					// Look up table which blockset are relevant. Roughly based on init_is_blkset_used_LUT() for no ndegraded mode
};

static void conf_init(struct t_conf *f) {
	memset(f, 0, sizeof(*f));
	f->role = ~0;			// invalid default
	f->D = 0;
	f->P = 0;
	f->stride = (1 << LOCK_CHANGE_STRIDE_SHIFT);
	f->ignore_unused = false;
}

static void conf_finalize(struct t_conf *f, u64 seg_dlba_start) {
	int i, n_copies = (1 + f->P);
	f->n_segs = (f->D + f->P);
	f->n_blksets_full_roll_change = ((u32)f->n_segs * (u32)f->stride);
	for (i = 0; i < n_copies; i++)
		f->lut[(i + f->role) % f->n_segs] = true;
	f->dlba_offset = f->n_blksets_full_roll_change - (u32)(seg_dlba_start % f->n_blksets_full_roll_change);
}

static void conf_print(struct t_conf *f) {
	int i, n_seg = f->D + f->P;
	printf("-conf: D+P = {%d,%d}, stride=%d, dlba_offset=%d, role=%d\n\tLUT= {", f->D, f->P, f->stride, f->dlba_offset, f->role);
	for (i = 0; i < n_seg; i++)
		printf("%d ", f->lut[i]);
	printf("}\n");
}
static bool conf_is_dlba_relevant(const struct t_conf *f, u64 dlba) {
	return (!f->is_active) || (f->lut[((dlba + f->dlba_offset) / f->stride) % f->n_segs]);		// No filtering by conf = all is relevant
}

/*********************************** Parameters *******************************/
const char *reset_help_str = "'L' = reset only the lock. Binfo untouched, 'D' = reset only the dbits. TxID/Lock untouched. 'A' = all u64 bits are reset, '-' = Dry run";
struct t_params {
	u32 verbose   : 8;			// Print progress or not: 0 - none, 1 - verbose on interesting blockset locks, 2 - verbose on all blocksets
	u32 reset_type: 8;			// If resetting values, this is the reset type. See 'reset_help_str'
	u32 fmt_json  : 1;			// is format json or text
	u32 do_reset  : 1;			// Should reset the lock table
	union nvmeib_lock_blkset_entry lock_reset_val;	// If 'reset' is true, use this u64 to write to lock entry and binfo
	struct t_filter_all filter;	// Allows to act on specific ranges of specific disks
	struct t_conf conf;				// Parameters related to volume configuration
};

#define __YES_NO(condition)	((condition)?'Y':'N')
#define SOFTWARE_VERSION_STR "Version: 1.82"

#define PARSE_RESET_U32_PARAM(dst) do {\
	if ((i+1) < argc) { \
		rv->do_reset = true; \
		char *endptr = NULL; \
		dst = (u32)strtoul(argv[++i], &endptr, 16); /*atoi(argv[i++]);*/ \
		(void)endptr; \
	} else { \
		abort_with_msg("missing param for %s\n", argv[i]); \
	} \
} while (0)

#define PARSE_CONF_U32_PARAM(dst) do {\
	if ((i+1) < argc) { \
		rv->conf.is_active = true; \
		char *endptr = NULL; \
		dst = (u32)strtoul(argv[++i], &endptr, 10); /*atoi(argv[i++]);*/ \
		(void)endptr; \
	} else { \
		abort_with_msg("missing param for %s\n", argv[i]); \
	} \
} while (0)

static void __parse_args(u32 argc, char *argv[], struct t_params *rv) {
	u32 i;
	rv->verbose = 1;				// Default values
	rv->fmt_json = rv->reset_type = rv->do_reset = false;
	rv->lock_reset_val = get_lock_default_reset_val();
	memset(&rv->filter, 0, sizeof(rv->filter));
	conf_init(&rv->conf);
	if (geteuid() != 0)
		abort_with_msg("This app must be run as sudo\n");

	for (i = 1; i < argc; i++) {
		if        (!strcmp(argv[i], "-json")) {
			rv->fmt_json = true;
			rv->verbose =  0;
		} else if (!strcmp(argv[i], "-silent")) {
			rv->verbose =  0;
		} else if (!strcmp(argv[i], "-verbose")) {
			rv->verbose =  2;
		} else if (!strncmp(argv[i], "-reset", 6)) {
			rv->do_reset = true;
			rv->reset_type = 'A';						// Backwards compatible for v1.6 and below
			if (argv[i][6] == '=')						// Parse type of reset
				rv->reset_type = argv[i][7];
		} else if (!strcmp(argv[i], "-set_txid")) {
			PARSE_RESET_U32_PARAM(rv->lock_reset_val.blkset_info.bits.txid);
		} else if (!strcmp(argv[i], "-set_dbits")) {
			PARSE_RESET_U32_PARAM(rv->lock_reset_val.blkset_info.bits.dirty);
		} else if (!strcmp(argv[i], "-set_lock")) {
			PARSE_RESET_U32_PARAM(rv->lock_reset_val.lock_id.all);
		} else if (!strcmp(argv[i], "-conf_d")) {
			PARSE_CONF_U32_PARAM(rv->conf.D);
		} else if (!strcmp(argv[i], "-conf_p")) {
			PARSE_CONF_U32_PARAM(rv->conf.P);
		} else if (!strcmp(argv[i], "-conf_role")) {
			PARSE_CONF_U32_PARAM(rv->conf.role);
		} else if (!strcmp(argv[i], "-conf_ignore_unused")) {
			rv->conf.ignore_unused = true;
			rv->conf.is_active = true;
		} else if (!strncmp(argv[i], FMT_FILTERS_HEADER, 8)) {
			disk_filters_init(&rv->filter, argv[i]);
		} else if (!strncmp(argv[i], "-h", 2)  ||			// -h, -help
				   !strncmp(argv[i], "--h", 3) ||			// --h, --help
				   !strncmp(argv[i], "-v", 2)) {			// -v, -version
			abort_with_msg(SOFTWARE_VERSION_STR "\nargs:\t-json -silent -reset -verbose -help\n"
						   "\tFilter example:\t%s\n"
						   "\tFilter values are typically taken from configuration of volume segment lba divided by %d\n"
						   "\tConfig example:\t-conf_d 8 -conf_p 2 -conf_role 3     -conf_ignore_unused\n"
						   "\tReset example:\t-reset -set_txid 0x17 -set_lock 0x11 -set_dbits 0x1\n"
						   "\tPartial reset:\t-reset=D -set_dbits 0x0,      -reset=L -set_lock 0x%x\n"
						   "\t\t%s\n"
						   "\n\nTo only print unique stale locks per disk:\n\t%s\n",
						   filters_example, NUM_BLKS_IN_BLKSET, nvmeib_stale_special_raid1.lock_id.all, reset_help_str, filter_all_unique_stale_locks_example);
		} else {
			abort_with_msg("unknown param: %s\n", argv[i]);
		}
	}

	if (rv->conf.is_active) {
		if ((!disk_filter_is_active(&rv->filter) || rv->filter.n_disks != 1 || rv->filter.disks[0].n_ranges != 1))
			abort_with_msg("Configuration filter can be used only with single segment filter (1 disk, 1 range)\n");
		if ((rv->conf.D == 0) || (rv->conf.P == 0) || (rv->conf.role >= (rv->conf.D+rv->conf.P))){
			conf_print(&rv->conf);
			abort_with_msg("Configuration filter is invalid!\n");
		}
		conf_finalize(&rv->conf, rv->filter.disks[0].range[0].start);
	}

	if (rv->verbose) {
		printf("\n" SOFTWARE_VERSION_STR "\n"
			   "Assumptions: Driver block size %d[bytes], %d blocks in blockset, blockset entry size %d[bytes]\n", DRV_BLK_SIZE, NUM_BLKS_IN_BLKSET, (int)NVMEIB_LOCK_BLKSET_ENTRY_SIZE);
		printf("Parmas:\n-fmt=%s, reset=%c, verbose=%d, use_filters=%c\n", (rv->fmt_json ? "json" : "txt"), __YES_NO(rv->do_reset), rv->verbose, __YES_NO(disk_filter_is_active(&rv->filter)));
		if (rv->do_reset) {
			printf("\tResetting blockset entry to ");
				printf("lock-id=0x%x, txid=0x%05x, dirty=0x%03x",
					   rv->lock_reset_val.lock_id.all, rv->lock_reset_val.blkset_info.bits.txid, rv->lock_reset_val.blkset_info.bits.dirty);
			if ((rv->reset_type != 'D') && (rv->reset_type != 'L') && (rv->reset_type != 'A'))
				rv->reset_type = '-';						// Unspecified defaults to dry run
			printf(". reset type: %c\n", rv->reset_type);
		}

		if (rv->filter.n_disks) 	// Done parsing, print filters for debug
			disk_filters_print(&rv->filter);
		if (rv->conf.is_active) 		// Done parsing, print filters for debug
			conf_print(&rv->conf);
	}
}

/************************* Disk locks representation **************************/
#define LINE_MAX_LEN (1024)
struct t_disk {
	const char *name;
	u64 n_blks, n_4k_blks, n_blksets;
	int disk_blk_size;
	size_t length;				// Length of mmap area of locks
	off_t offset;				// Offset of mmap area of locks
	char mmap_path[256];
	int prot;					// Type of mmap protection
};
static void t_disk_parse_from_string(char line[LINE_MAX_LEN], struct t_disk *d) {	 // parse line describing a singe disk
	char *token_data, *token_header;
	char *sptr_data, *sptr_header;
	char *header = strdup(NVMEIBS_DISKS_CSV_HEADER);
	for (token_header = strtok_r(header, ",", &sptr_header), token_data = strtok_r(line, ",", &sptr_data); token_data && token_header;) {
		if (!strcmp(token_header, "id")) {
			d->name = token_data;
		} else if (!strcmp(token_header, "blocks")) {
			d->n_blks = atoll(token_data);
		} else if (!strcmp(token_header, "block_size")) {
			d->disk_blk_size = atoi(token_data);
			break; // Optimization; we know this is the last field.
		}
		token_header = strtok_r(NULL, ",", &sptr_header);
		token_data = strtok_r(NULL, ",", &sptr_data);
	}
	free(header);

	if (d->name == NULL || d->n_blks == 0 || d->disk_blk_size == 0)
		abort_with_msg("Failed to parse disk line: %s\n", line);

	// calc n_blksets
	d->n_4k_blks = (d->disk_blk_size <= DRV_BLK_SIZE) ?
		d->n_blks / (DRV_BLK_SIZE / d->disk_blk_size) :
		d->n_blks * (d->disk_blk_size / DRV_BLK_SIZE);
	d->n_blksets = DIV_ROUND_UP(d->n_4k_blks, NUM_BLKS_IN_BLKSET);

	// Array of mmap
	d->length = d->n_blksets * NVMEIB_LOCK_BLKSET_ENTRY_SIZE;
	d->offset = 0;
	snprintf(d->mmap_path, 256, "/proc/nvmeibs/locks.%s", d->name);
}

static void t_disk_to_string(const struct t_disk *d, u32 verbose) {
	if (verbose)
		printf("\n### Disk %s\nmmap_path: %s\nmmap locks table (length 0x%lx, offset 0x%lx), 0x%llx locks ...\n", d->name, d->mmap_path, d->length, d->offset, d->n_blksets);
}

/***************************** Summary stats **********************************/
struct t_stats {
	u64 num_stales; 					// All stale locks including special
	u64 num_stales_specials;			// For R1 only
	u64 num_taken;						// Num non zero no stale locks
	u64 num_dbits, num_dbits_unk, num_txid_unk, num_resets;
	u64 num_unused_non_zero;			// Number of unused blockset entries which should always remain zero but were corrupted. Memory corruption?
};

/******************************************************************************/
#define FORMAT_LOCK       " 0x%012llx: all=0x%016llx, lock-id=0x%06x:%x, is_stale=%d, is_read=%d"
#define FORMAT_BINFO_EC   ", txid=0x%05x, dirty=0x%03x"
static void __blockset_entry_process(union nvmeib_lock_blkset_entry *lock_p, u64 ii, const struct t_params *p, struct t_stats *stats) {
	const union nvmeib_lock_blkset_entry l = lock_p[ii];
	const u32 is_interesting = (l.lock_id.all || l.blkset_info.bits.dirty);
	if (!p->verbose) {
		// Print nothing
	} else if ( (p->verbose == 2) ||
			   ((p->verbose == 1) && (is_interesting))) {
			printf("Blkset" FORMAT_LOCK FORMAT_BINFO_EC "\n",
			   ii, l.all, l.lock_id.bits.lock_id, l.lock_id.bits.idx_in_praid,
			   l.lock_id.bits.is_stale, l.lock_id.bits.is_read,
			   l.blkset_info.bits.txid, l.blkset_info.bits.dirty);
	}
	if (	 l.lock_id.bits.is_stale) {
		stats->num_stales++;
		if (l.lock_id.all == nvmeib_stale_special_raid1.all)
			stats->num_stales_specials++;
	} else if (l.lock_id.all) {
		stats->num_taken++;
	}
	if (l.blkset_info.bits.dirty) {
		const union nvmeibc_dbits_entry e = {.all_bits = l.blkset_info.bits.dirty };
		stats->num_dbits++;
		if (nvmeibc_dbits_has_unknowns(&e))
			stats->num_dbits_unk++;
	}
	if (l.blkset_info.bits.txid == INITIAL_LAZY_READ_TXID) {
		stats->num_txid_unk++;
	}
	if ((p->do_reset) && (l.all != p->lock_reset_val.all)) {
		if (p->reset_type == 'A') {
			lock_p[ii].all = p->lock_reset_val.all;
		} else if (p->reset_type == 'L') {
			lock_p[ii].lock_id.all = p->lock_reset_val.lock_id.all;
		} else if (p->reset_type == 'D') {
			lock_p[ii].blkset_info.bits.dirty = p->lock_reset_val.blkset_info.bits.dirty;
		} // else: Dry run, do nothing
		stats->num_resets++;
	}
}

static void __blockset_entry_verify_zero(union nvmeib_lock_blkset_entry *lock_p, u64 ii, const struct t_params *p, struct t_stats *stats) {
	const union nvmeib_lock_blkset_entry l = lock_p[ii];
	const u32 is_interesting = (l.lock_id.all != 0);
	if (!p->verbose) {
		// Print nothing
	} else if ( (p->verbose == 2) ||
			   ((p->verbose == 1) && (is_interesting))) {
			printf("------" FORMAT_LOCK FORMAT_BINFO_EC "\n",
			   ii, l.all, l.lock_id.bits.lock_id, l.lock_id.bits.idx_in_praid,
			   l.lock_id.bits.is_stale, l.lock_id.bits.is_read,
			   l.blkset_info.bits.txid, l.blkset_info.bits.dirty);
	}
	if (is_interesting) {
		stats->num_unused_non_zero++;		// possible memory corruptions
	}
}

int main(int argc, char *argv[])
{
	char disks_path[] = "/proc/nvmeibs/disks.csv";
	FILE *disks_fp;
	int ll;
	char line[LINE_MAX_LEN];

	int mmap_fd;
	char *addr;
	struct t_disk disk = {0};
	struct t_stats stats;
	struct t_params p;
	u64 ii = 0;

	__parse_args((u32)argc, argv, &p);
	if (!(disks_fp = fopen(disks_path, "r")))		// disks
		abort_with_msg("Failed to open %s\n", disks_path);

	if (p.fmt_json)
		printf("{\n");
	ll = 0;
	while (fgets(line, LINE_MAX_LEN, disks_fp) != NULL) {
		if (!ll++) continue;				// Skip first line
		t_disk_parse_from_string(line, &disk);
		if (disk_filters_should_skip_disk(&p.filter, disk.name))
			continue;						// No need to process this disk

		t_disk_to_string(&disk, p.verbose);
		// Do mmap
		if ((mmap_fd = open(disk.mmap_path, O_RDWR)) < 0)
			abort_with_msg("Failed to open %s\n", disk.mmap_path);

		disk.prot = (PROT_READ | (p.do_reset ? PROT_WRITE : 0));		// write permissions to reset locks
		addr = mmap(NULL, disk.length, disk.prot , MAP_SHARED, mmap_fd, disk.offset);
		if (addr == MAP_FAILED)
			handle_error("mmap");

		// scan /change
		memset(&stats, 0, sizeof(stats));
		if (disk_filter_is_active(&p.filter)) {
			for (ii = filter_rng_start(&p.filter, disk.n_blksets); filter_rng_has_next(&p.filter); ii = filter_rng_inc(&p.filter)) { // ii In units of dlba_blksets
				if (conf_is_dlba_relevant(&p.conf, ii))
					__blockset_entry_process((void*)addr, ii, &p, &stats);
				else if (!p.conf.ignore_unused) {
					__blockset_entry_verify_zero((void*)addr, ii, &p, &stats);
				}
			}
		} else {
			for (ii = 0; ii < disk.n_blksets; ii++)
					__blockset_entry_process((void*)addr, ii, &p, &stats);
		}
		if (p.fmt_json) {
			printf("\"disk_%d\" : {\"name\" : \"%s\", \"stales\" : %llu, \"stalessp\" : %llu, \"taken\" : %llu, \"dbits\" : %llu, \"unk_dbits\" : %llu, \"unk_txid\" : %llu, \"resets\" : %llu, \"mem_corrupt\" : %llu},\n", ll-1,
				   disk.name, stats.num_stales, stats.num_stales_specials, stats.num_taken, stats.num_dbits, stats.num_dbits_unk, stats.num_txid_unk, stats.num_resets, stats.num_unused_non_zero);
		} else {
			printf("scan done. {disk=%-26s, stales=%020llu, stalessp=%020llu, taken=%020llu, dbits=%020llu, ", disk.name, stats.num_stales, stats.num_stales_specials, stats.num_taken, stats.num_dbits);
			printf("unk{db=%020llu, txid=%020llu}, resets=%020llu, ", stats.num_dbits_unk, stats.num_txid_unk, stats.num_resets);
			if (stats.num_unused_non_zero)
				printf(KERN_COL_RED_BL_BOLD "mem_corrupt=%020llu" KERN_COL_RESET, stats.num_unused_non_zero);
			else
				printf(                     "mem_corrupt=%020llu"               , stats.num_unused_non_zero);
			printf("}\n");
		}

		if (munmap(addr, disk.length) == -1)
			handle_error("munmap");
		close(mmap_fd);
	}
	if (p.fmt_json) printf("\"num_disks\" : %u\n}\n", ll-1);

	fclose(disks_fp);
	return 0;
}
