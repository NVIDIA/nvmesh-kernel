/**
 * gpt_util - NVMesh GPT/MBR Utility
 *
 * Displays, exports, and applies GPT structures for NVMesh-managed and regular block devices.
 */

#include <ctype.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gpt_util_self_test.h"

#include "../nvmeibt_debug.h"
#include "../nvmeibt_disk_metadata.h"
#include "nvmeibt_bm.h"
#include "../nvmeibt_read_config.h"
#include "nvmeibt_str.h"
#include "../nvmeibt_local_disk.h"
#include "nvmeibt_uuid.h"
#include "../nvmeibt_json_base.h"
#include "../interfaces/log/nvmeibt_binary_tracing.h"

#define GPT_UTIL_VERSION	"2.0.0-dev"
#define MAX_DEV_NAME		256


// Actions (mutually exclusive operations)
enum GPT_UTIL_ACTION {
	ACTION_DISPLAY_GPT = 0,		// Default: display GPT structure
	ACTION_DISPLAY_MBR,			// -m: display MBR only
	ACTION_FIX_GPT,				// -f: fix GPT from alternate copy
	ACTION_FIX_MBR,				// -F: fix MBR
	ACTION_CHECK_EXCELERO,		// -i: check if EXCELERO_METADATA exists
	ACTION_EXPORT_JSON,			// --output-json: export GPT to JSON
	ACTION_APPLY_JSON,			// --apply-from: apply GPT from JSON (dry-run by default)
	ACTION_UPGRADE_GPT			// -U: upgrade GPT (fix n_partition_entries to 8192 and recalculate CRC)
};

// GPT level (Main or Metadata)
enum GPT_LEVEL {
	GPT_LEVEL_MAIN = 0,
	GPT_LEVEL_METADATA
};

// GPT copy (Primary or Alternate)
enum GPT_COPY {
	GPT_COPY_PRIMARY = 0,
	GPT_COPY_ALTERNATE
};

// GPT copy option for the execution
enum GPT_COPY_OPTION {
	GPT_COPY_OPTION_PRIMARY = 0x1,		// Bit 0
	GPT_COPY_OPTION_ALTERNATE = 0x2,	// Bit 1
	GPT_COPY_OPTION_BOTH = 0x3			// Both bits (PRIMARY | ALTERNATE)
};

// Enum-to-string conversions (ternary for binary choices - better for branch predictor)
static const char *gpt_level_str(enum GPT_LEVEL level)
{
	return (level == GPT_LEVEL_MAIN) ? "main" : "metadata";
}

static const char *gpt_copy_str(enum GPT_COPY copy)
{
	return (copy == GPT_COPY_PRIMARY) ? "primary" : "alternate";
}

static const char *gpt_copy_option_str(enum GPT_COPY_OPTION option)
{
	switch (option) {
	case GPT_COPY_OPTION_PRIMARY:	return "primary";
	case GPT_COPY_OPTION_ALTERNATE:	return "alternate";
	case GPT_COPY_OPTION_BOTH:		return "both";
	default:						return "unknown";
	}
}

// O_DIRECT mode (for device I/O)
enum O_DIRECT_MODE {
	O_DIRECT_AUTO = 0,			// Auto-detect: block device=yes, regular file=no
	O_DIRECT_FORCE_ON,			// Force O_DIRECT even for files (may fail with EINVAL)
	O_DIRECT_FORCE_OFF			// Disable O_DIRECT even for block devices
};

// Configuration structure for gpt_util operation
struct gpt_util_config {
	// Action
	enum GPT_UTIL_ACTION	action;

	// Device context
	char					device_path[MAX_DEV_NAME];
	BOOL					is_nvmesh_managed;		// true=-d, false=-a
	uint64_t				pba_s;
	uint64_t				pba_e;
	uint64_t				pba_hw_e;
	int						pblk_size;

	// Display/export options
	enum GPT_COPY_OPTION	gpt_copy_option;		// primary|alternate|both (for both display and export)

	// Filtering options
	char					filter_uuid_str[64];	// Filter by UUID (empty = no filter)
	uint64_t				filter_lba;				// Filter by LBA (0 = no filter)
	BOOL					has_uuid_filter;
	BOOL					has_lba_filter;

	// Zeroing verification
	BOOL					print_zero_verify_cmds;	// -Z: print zeroing verification commands

	// JSON export options (for ACTION_EXPORT_JSON)
	char					output_json_file[256];	// Output JSON filename

	// JSON apply options (for ACTION_APPLY_JSON)
	char					apply_json_file[256];	// Input JSON filename to apply
	BOOL					write_mode;				// true = write changes, false = dry-run (default)

	// I/O options
	enum O_DIRECT_MODE		o_direct_mode;			// O_DIRECT behavior
};

// Check if operation is read-only (no disk modifications)
// Considers both action type and mode (e.g., dry-run vs write)
static BOOL is_action_read_only(struct gpt_util_config *config)
{
	switch (config->action) {
	case ACTION_DISPLAY_GPT:
	case ACTION_DISPLAY_MBR:
	case ACTION_CHECK_EXCELERO:
	case ACTION_EXPORT_JSON:
		return true;
	case ACTION_APPLY_JSON:
		return !config->write_mode;		// Dry-run = readonly, write = readwrite
	case ACTION_FIX_GPT:
	case ACTION_FIX_MBR:
	case ACTION_UPGRADE_GPT:
		return false;
	default:
		return false;
	}
}

// Determine if we should use O_DIRECT based on file type and user preference
static BOOL should_use_o_direct(const char *dev_path, enum O_DIRECT_MODE mode)
{
	struct stat st;

	// Explicit override
	if (mode == O_DIRECT_FORCE_ON) {
		return true;
	}
	if (mode == O_DIRECT_FORCE_OFF) {
		return false;
	}

	// Auto-detect based on file type
	if (stat(dev_path, &st) < 0) {
		// If stat fails, default to safe choice (no O_DIRECT)
		return false;
	}

	// Block device: use O_DIRECT (preserves production behavior)
	if (S_ISBLK(st.st_mode)) {
		return true;
	}

	// Regular file: don't use O_DIRECT (avoids EINVAL due to alignment issues)
	return false;
}

/**
 * Open target device with appropriate flags based on action and file type
 * Returns fd on success, -1 on error
 */
static int open_target_device(struct gpt_util_config *config)
{
	int		open_flags = O_EXCL;
	BOOL	use_o_direct;
	BOOL	is_readonly;
	int		fd;

	use_o_direct = should_use_o_direct(config->device_path, config->o_direct_mode);
	is_readonly = is_action_read_only(config);

	// Determine access mode
	open_flags |= (is_readonly ? O_RDONLY : O_RDWR);

	// Add O_DIRECT if appropriate
	if (use_o_direct) {
		open_flags |= __O_DIRECT;
	}

	N_Tf(open_device_flags, "Opening dev=@STR flags=@X (readonly=@INT o_direct=@INT)",
		 config->device_path, open_flags, is_readonly, use_o_direct);

	fd = open(config->device_path, open_flags);
	if (fd < 0) {
		N_Ef(run_gpt_util_open_failed, "Unable to open @STR flags=@X @AUTO_ERRNO",
			 config->device_path, open_flags);
		return -1;
	}

	return fd;
}



// GPT buffer set for reading both primary and alternate copies
struct gpt_buffers {
	struct nvmeibt_disk_gpt_header			*primary_header;
	struct nvmeibt_disk_gpt_header			*alternate_header;
	struct nvmeibt_disk_gpt_partition_entry	*primary_entries;
	struct nvmeibt_disk_gpt_partition_entry	*alternate_entries;
};

// Allocate GPT buffers for reading both copies
static void alloc_gpt_buffers(struct gpt_buffers *bufs, int pblk_size, int max_n_entries)
{
	int		n_bytes_header;
	int		n_bytes_entries;

	n_bytes_header = roundup(sizeof(struct nvmeibt_disk_gpt_header), pblk_size);
	n_bytes_entries = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * max_n_entries, pblk_size);

	bufs->primary_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_buf_pri_hdr, PAGE_SIZE, n_bytes_header);
	bufs->alternate_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_buf_alt_hdr, PAGE_SIZE, n_bytes_header);
	bufs->primary_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_buf_pri_ent, PAGE_SIZE, n_bytes_entries);
	bufs->alternate_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_buf_alt_ent, PAGE_SIZE, n_bytes_entries);
}

// Free GPT buffers
static void free_gpt_buffers(struct gpt_buffers *bufs)
{
	NNVMEIBT_BM_FREE(trace_gpt_buf_cleanup_pri_hdr, bufs->primary_header);
	NNVMEIBT_BM_FREE(trace_gpt_buf_cleanup_alt_hdr, bufs->alternate_header);
	NNVMEIBT_BM_FREE(trace_gpt_buf_cleanup_pri_ent, bufs->primary_entries);
	NNVMEIBT_BM_FREE(trace_gpt_buf_cleanup_alt_ent, bufs->alternate_entries);
}
static BOOL entries_are_equal(const struct nvmeibt_disk_gpt_partition_entry *e1,
							  const struct nvmeibt_disk_gpt_partition_entry *e2)
{
	// Compare all fields
	if (!ARE_UUID_EQ(&e1->partition_type_guid, &e2->partition_type_guid)) return false;
	if (!ARE_UUID_EQ(&e1->partition_guid, &e2->partition_guid)) return false;
	if (e1->pba_s != e2->pba_s) return false;
	if (e1->pba_e != e2->pba_e) return false;
	if (e1->attributes != e2->attributes) return false;

	// Compare names (UTF-16)
	if (memcmp(e1->partition_name, e2->partition_name, sizeof(e1->partition_name)) != 0) return false;

	return true;
}

/**
 * Show diff for a single GPT entry
 */
static void show_entry_diff(const char *change_type,
							int index,
							const struct nvmeibt_disk_gpt_partition_entry *old_entry,
							const struct nvmeibt_disk_gpt_partition_entry *new_entry)
{
	char old_name[GPT_MAX_PARTITION_NAME_LENGTH + 1] = {0};
	char new_name[GPT_MAX_PARTITION_NAME_LENGTH + 1] = {0};
	struct nvmeibt_urn_uuid type_uuid;

	if (old_entry) {
		char16_str_to_str(old_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, old_name);
	}
	if (new_entry) {
		char16_str_to_str(new_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, new_name);
	}

	fprintf(stdout, "\n  " COL_YELLOW "%s Entry %d:" COL_RESET "\n", change_type, index);

	if (old_entry && new_entry) {
		// Modified
		if (strcmp(old_name, new_name) != 0) {
			fprintf(stdout, "    Name: %s -> %s\n", old_name, new_name);
		}
		if (old_entry->pba_s != new_entry->pba_s || old_entry->pba_e != new_entry->pba_e) {
			fprintf(stdout, "    Range: %lu-%lu -> %lu-%lu\n",
					old_entry->pba_s, old_entry->pba_e,
					new_entry->pba_s, new_entry->pba_e);
		}
		if (old_entry->attributes != new_entry->attributes) {
			fprintf(stdout, "    Attributes: 0x%lx -> 0x%lx\n",
					old_entry->attributes, new_entry->attributes);
		}
	} else if (new_entry) {
		// Added
		type_uuid = nvmeibt_union_uuid_to_urn_uuid(&new_entry->partition_type_guid);
		fprintf(stdout, "    Name: %s\n", new_name);
		fprintf(stdout, "    Type: %s\n", type_uuid.str);
		fprintf(stdout, "    Range: %lu-%lu\n", new_entry->pba_s, new_entry->pba_e);
	} else {
		// Deleted
		type_uuid = nvmeibt_union_uuid_to_urn_uuid(&old_entry->partition_type_guid);
		fprintf(stdout, "    Name: %s\n", old_name);
		fprintf(stdout, "    Type: %s\n", type_uuid.str);
		fprintf(stdout, "    Range: %lu-%lu\n", old_entry->pba_s, old_entry->pba_e);
	}
}

/**
 * Check if two partition entries overlap in LBA range
 * Returns true if they overlap
 */
static BOOL entries_overlap(const struct nvmeibt_disk_gpt_partition_entry *e1,
							const struct nvmeibt_disk_gpt_partition_entry *e2)
{
	// Check if both entries are in use
	if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(e1) ||
		!nvmeibt_disk_metadata_is_gpt_entry_in_use(e2)) {
		return false;
	}

	// Check for overlap: e1.start <= e2.end AND e2.start <= e1.end
	return (e1->pba_s <= e2->pba_e && e2->pba_s <= e1->pba_e);
}

/**
 * Detect overlapping partition entries in GPT
 * Returns number of overlaps found
 */
static int detect_overlaps(const struct nvmeibt_disk_gpt_partition_entry *entries,
						   int max_n_entries)
{
	int		i;
	int		j;
	int		n_overlaps = 0;

	for (i = 0; i < max_n_entries; i++) {
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i])) {
			continue;
		}

		for (j = i + 1; j < max_n_entries; j++) {
			if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[j])) {
				continue;
			}

			if (entries_overlap(&entries[i], &entries[j])) {
				fprintf(stdout, COL_YELLOW "WARNING: Overlap detected between entry %d and entry %d" COL_RESET "\n", i, j);
				fprintf(stdout, "  Entry %d: LBA %lu-%lu\n", i, entries[i].pba_s, entries[i].pba_e);
				fprintf(stdout, "  Entry %d: LBA %lu-%lu\n", j, entries[j].pba_s, entries[j].pba_e);
				n_overlaps++;
			}
		}
	}

	return n_overlaps;
}

/**
 * Print commands to verify a partition range is properly deleted/zeroed
 * Includes both data payload and data metadata (DMD) verification
 * Useful after segment deletion or for troubleshooting
 */
static void print_zero_verify_commands(const char *device_path,
									   const struct nvmeibt_disk_gpt_partition_entry *entry,
									   int entry_idx,
									   const char *gpt_level)
{
	uint64_t	first_lba = entry->pba_s;
	uint64_t	mid_lba = (entry->pba_s + entry->pba_e) / 2;
	uint64_t	last_lba = entry->pba_e;
	char		partition_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];

	// Convert partition name from UTF-16
	char16_str_to_str(entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name);

	fprintf(stdout, "\n");
	fprintf(stdout, COL_BLUE "--- Deletion Verification: %s Entry %d (%s) ---" COL_RESET "\n",
			gpt_level, entry_idx, partition_name);
	fprintf(stdout, COL_YELLOW "# Step 1: Verify Data Metadata (DMD) is cleared (quick spot-check):" COL_RESET "\n");
	fprintf(stdout, "# Checks: Version/EDIC/TxID/JRI Metadata fields are reset\n");
	fprintf(stdout, "tools/rw_dmd.sh %s %lu           # First block metadata\n", device_path, first_lba);
	fprintf(stdout, "tools/rw_dmd.sh %s %lu          # Middle block metadata\n", device_path, mid_lba);
	fprintf(stdout, "tools/rw_dmd.sh %s %lu          # Last block metadata\n", device_path, last_lba);
	fprintf(stdout, "\n");
	fprintf(stdout, COL_YELLOW "# Step 2: Verify Data Payload is zeroed (thorough full-range check):" COL_RESET "\n");
	fprintf(stdout, "# Checks: Actual block content is all zeros\n");
	fprintf(stdout, "source tools/block_team_bashrc.sh\n");
	fprintf(stdout, "NVMESH_io_check_zero %s %lx %lx   # LBA %lu-%lu (%lu blocks)\n",
			device_path, first_lba, last_lba, first_lba, last_lba, (last_lba - first_lba + 1));
	fprintf(stdout, "\n");
}

/**
 * Check if partition entry matches filter criteria
 * Returns true if entry should be displayed
 */
static BOOL matches_filter(const struct nvmeibt_disk_gpt_partition_entry *entry,
						   const struct gpt_util_config *config)
{
	union nvmeib_uuid	entry_uuid;
	union nvmeib_uuid	filter_uuid;

	// No filters = show all
	if (!config->has_uuid_filter && !config->has_lba_filter) {
		return true;
	}

	// UUID filter
	if (config->has_uuid_filter) {
		entry_uuid = entry->partition_guid;
		// Parse filter UUID string
		if (nvmeibt_urn_uuid_str_to_union_uuid(&filter_uuid, config->filter_uuid_str) < 0) {
			return false;		// Invalid filter UUID
		}
		if (!ARE_UUID_EQ(&entry_uuid, &filter_uuid)) {
			return false;
		}
	}

	// LBA filter
	if (config->has_lba_filter) {
		if (config->filter_lba < entry->pba_s || config->filter_lba > entry->pba_e) {
			return false;		// LBA not in this partition range
		}
	}

	return true;
}

/**
 * Display a single GPT copy with validity markers, optional mismatch flag, and filtering
 */
static void display_gpt_one_copy(const char *gpt_level,
							 const char *copy_name,
							 enum GPT_VALIDITY header_validity,
							 enum GPT_VALIDITY entries_validity,
							 const struct nvmeibt_disk_gpt_partition_entry *entries,
							 int max_n_entries,
							 BOOL is_mismatch,
							 const struct gpt_util_config *config)
{
	struct nvmeibt_Str	*outstr;
	int					i;
	int					n_displayed = 0;

	if (is_mismatch) {
		fprintf(stdout, "\n=== %s %s [Head=%s Entries=%s] [MISMATCH] ===\n",
				gpt_level, copy_name,
				gpt_validity_str(header_validity),
				gpt_validity_str(entries_validity));
	} else {
		fprintf(stdout, "\n=== %s %s [Head=%s Entries=%s] ===\n",
				gpt_level, copy_name,
				gpt_validity_str(header_validity),
				gpt_validity_str(entries_validity));
	}

	// If filtering, display entries one by one with filter check
	if (config && (config->has_uuid_filter || config->has_lba_filter)) {
		fprintf(stdout, "%s-%s-GPT Active Entries (filtered):\n", gpt_level, copy_name);
		fprintf(stdout, " Idx type_uuid                         partition_guid                         pba_s      pba_e  attributes name\n");

		for (i = 0; i < max_n_entries; i++) {
			if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i]) &&
				matches_filter(&entries[i], config)) {
				outstr = NNVMEIBT_STR_ALLOC(trace_gpt_util_display_filtered);
				nvmeibt_disk_metadata_fill_gpt_entry_str(&entries[i], i, outstr);
				fprintf(stdout, "%s", nvmeibt_Str_str(outstr));
				NNVMEIBT_STR_FREE(trace_gpt_util_display_filtered_free, outstr);
				n_displayed++;

				// Print zeroing verification commands if requested
				if (config && config->print_zero_verify_cmds) {
					print_zero_verify_commands(config->device_path, &entries[i], i, gpt_level);
				}
			}
		}

		fprintf(stdout, "Total entries displayed: %d\n", n_displayed);
	} else {
		// No filtering - use existing function
		outstr = NNVMEIBT_STR_ALLOC(trace_gpt_util_display_copy);
		nvmeibt_disk_metadata_fill_all_gpt_entries_str(entries, max_n_entries, true,
													   outstr, gpt_level, copy_name);
		fprintf(stdout, "%s\n", nvmeibt_Str_str(outstr));
		NNVMEIBT_STR_FREE(trace_gpt_util_display_copy_free, outstr);

		// Print zeroing verification commands if requested
		if (config && config->print_zero_verify_cmds) {
			for (i = 0; i < max_n_entries; i++) {
				if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i])) {
					print_zero_verify_commands(config->device_path, &entries[i], i, gpt_level);
				}
			}
		}
	}

	// Detect and display overlaps (only if entries are valid)
	if (entries_validity == GPT_VALIDITY_OK) {
		int n_overlaps = detect_overlaps(entries, max_n_entries);
		if (n_overlaps > 0) {
			fprintf(stdout, "\n" COL_YELLOW "*** WARNING: %d overlap(s) detected in %s %s ***" COL_RESET "\n",
					n_overlaps, gpt_level, copy_name);
		}
	}
}

/**
 * Read and display GPT copies (primary and/or alternate) for one GPT level
 */
static void display_gpt_copies_one_level(const char *gpt_level,
										 enum GPT_COPY_OPTION gpt_copy_option,
										 int disk_fd,
										 int pblk_size,
										 uint64_t pba_s,
										 uint64_t pba_e,
										 int max_n_entries,
										 const struct gpt_util_config *config)
{
	struct gpt_buffers						bufs;
	enum GPT_VALIDITY						primary_header_validity;
	enum GPT_VALIDITY						alternate_header_validity;
	enum GPT_VALIDITY						primary_entries_validity;
	enum GPT_VALIDITY						alternate_entries_validity;
	struct nvmeibt_disk_gpt					temp_gpt;
	BOOL									is_header_mismatch;
	BOOL									is_entries_mismatch;
	BOOL									is_mismatch;

	// Allocate buffers
	alloc_gpt_buffers(&bufs, pblk_size, max(LARGE_GPT_MAX_NUM_GPT_ENTRIES, MAX_NUM_GPT_ENTRIES));

	memset(&temp_gpt, 0, sizeof(temp_gpt));
	temp_gpt.max_n_entries = max_n_entries;
	nvmeibt_strlcpy(temp_gpt.main_or_metadata, gpt_level, sizeof(temp_gpt.main_or_metadata));

	// Read all 4 structures
	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
		NULL, disk_fd, pblk_size, &temp_gpt, pba_s, pba_e,
		&primary_header_validity, &alternate_header_validity,
		&primary_entries_validity, &alternate_entries_validity,
		bufs.primary_header, bufs.alternate_header,
		bufs.primary_entries, bufs.alternate_entries);

	// Detect mismatch: both copies valid but differ in content
	is_header_mismatch = false;
	is_entries_mismatch = false;

	if (primary_header_validity == GPT_VALIDITY_OK &&
		alternate_header_validity == GPT_VALIDITY_OK) {
		is_header_mismatch = !nvmeibt_disk_metadata_are_gpt_headers_equal(bufs.primary_header, bufs.alternate_header);
	}

	if (primary_entries_validity == GPT_VALIDITY_OK &&
		alternate_entries_validity == GPT_VALIDITY_OK &&
		bufs.primary_header->n_partition_entries > 0) {
		is_entries_mismatch = !nvmeibt_disk_metadata_are_gpt_entries_equal(
			bufs.primary_entries, bufs.alternate_entries, bufs.primary_header->n_partition_entries * sizeof(struct nvmeibt_disk_gpt_partition_entry));
	}

	is_mismatch = (is_header_mismatch || is_entries_mismatch);

	// Display based on --gpt-copy option
	if (gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
		display_gpt_one_copy(gpt_level, "Primary", primary_header_validity,
						primary_entries_validity, bufs.primary_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
	}
	if (gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		display_gpt_one_copy(gpt_level, "Alternate", alternate_header_validity,
						alternate_entries_validity, bufs.alternate_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
	}

	// Free buffers
	free_gpt_buffers(&bufs);
}

/**
 * Display all GPT information for a device (complete GPT view)
 * This function is the production code path for displaying GPT.
 * Used by both normal operation and self-test.
 *
 * @param disk_fd       File descriptor of the device
 * @param pblk_size     Physical block size
 * @param pba_s         Start PBA
 * @param pba_hw_e      Hardware end PBA
 * @param gpt_copy_option  Which copy to display (primary, alternate, or both)
 * @param dev_name      Device name for error messages
 * @param fix_gpt       Whether to attempt GPT fix
 * @param config        Config for filtering (can be NULL for no filtering)
 * @return              0 on success, -1 on failure
 */
static int display_all_gpts(int disk_fd,
							int pblk_size,
							uint64_t pba_s,
							uint64_t pba_hw_e,
							enum GPT_COPY_OPTION gpt_copy_option,
							const char *dev_name,
							BOOL fix_gpt,
							const struct gpt_util_config *config)
{
	int										rv = -1;
	struct nvmeibt_disk_gpt					main_gpt;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_entry;

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));

	// Read and display Main GPT copies
	display_gpt_copies_one_level("Main", gpt_copy_option, disk_fd, pblk_size,
								pba_s, pba_hw_e, LARGE_GPT_MAX_NUM_GPT_ENTRIES, config);

	// For metadata GPT reading, need the best Main GPT copy
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, pblk_size, &main_gpt,
										  pba_s, pba_hw_e, fix_gpt) < 0) {
		N_Ef(display_restore_main_gpt_failed, "Unable to restore Main-GPT for metadata access. dev=@STR", dev_name);
		goto out;
	}

	// Try to fix the metadata GPT as well
	metadata_entry = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_entry) {
		fprintf(stdout, "No metadata entry found in main GPT (non-NVMesh disk)\n");
		rv = 0;		// Main GPT was displayed successfully
		goto out;
	}

	// Read and display Metadata GPT copies
	display_gpt_copies_one_level("Metadata", gpt_copy_option, disk_fd, pblk_size,
								metadata_entry->pba_s, metadata_entry->pba_e,
								MAX_NUM_GPT_ENTRIES, config);

	rv = 0;
out:
	return rv;
}

static void print_version_banner(void)
{
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_WHITE_BOLD "gpt_util - NVMesh GPT/MBR Utility" COL_RESET "\n");
	fprintf(stdout, "Tool Version: " COL_GREEN "%s" COL_RESET "\n", GPT_UTIL_VERSION);
#ifdef BUILD_VERSION_FOR_MGMT
	fprintf(stdout, "TOMA Version: %s", BUILD_VERSION_FOR_MGMT);
#ifdef BUILD_NUMBER_FOR_MGMT
	fprintf(stdout, " Build: %s", BUILD_NUMBER_FOR_MGMT);
#endif // #ifdef BUILD_NUMBER_FOR_MGMT
	fprintf(stdout, "\n");
#endif // #ifdef BUILD_VERSION_FOR_MGMT
#ifdef GIT_BRANCH
	fprintf(stdout, "Git Branch: %s", GIT_BRANCH);
#ifdef GIT_COMMIT_ID
	fprintf(stdout, " Commit: %s", GIT_COMMIT_ID);
#endif // #ifdef GIT_COMMIT_ID
	fprintf(stdout, "\n");
#endif // #ifdef GIT_BRANCH
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
}

// Export one GPT copy to JSON (helper function)
static void export_gpt_copy_entries_to_json(enum GPT_LEVEL level,
											enum GPT_COPY copy,
											const struct nvmeibt_disk_gpt_header *header,
											const struct nvmeibt_disk_gpt_partition_entry *entries,
											int max_n_entries,
											struct nvmeibt_Str *json_output,
											BOOL is_last_section)
{
	struct nvmeibt_urn_uuid	urn_uuid;
	int						i;
	int						entry_count = 0;

	// Build section name from enums using helper functions
	nvmeibt_Str_sprintf(json_output, "  \"%s_gpt_%s\": {\n",
						gpt_level_str(level), gpt_copy_str(copy));

	// Editable fields
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&header->disk_obj_uuid);
	nvmeibt_Str_sprintf(json_output, "    \"disk_uuid\": \"%s\",\n", urn_uuid.str);
	nvmeibt_Str_sprintf(json_output, "    \"n_partition_entries\": %d,\n", header->n_partition_entries);
	nvmeibt_Str_sprintf(json_output, "    \"first_usable_pba\": %lu,\n", header->first_usable_pba);
	nvmeibt_Str_sprintf(json_output, "    \"last_usable_pba\": %lu,\n", header->last_usable_pba);

	// Static fields (UEFI constants - do not edit)
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_gpt_signature\": \"0x%lx\",\n", header->gpt_signature);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_revision\": \"0x%08x\",\n", header->revision);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_header_size\": %d,\n", header->header_size);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_size_of_partition_entry\": %d,\n", header->size_of_partition_entry);

	// Computed fields (recalculated on write - do not edit)
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_header_crc32\": \"0x%08x\",\n", header->header_crc32);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_partition_entry_array_crc32\": \"0x%08x\",\n", header->partition_entry_array_crc32);

	nvmeibt_Str_sprintf(json_output, "    \"entries\": [\n");

	// Export partition entries
	for (i = 0; i < max_n_entries; i++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i])) {
			const struct nvmeibt_disk_gpt_partition_entry *entry = &entries[i];
			struct nvmeibt_urn_uuid type_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_type_guid);
			struct nvmeibt_urn_uuid part_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_guid);
			char partition_name_str[GPT_MAX_PARTITION_NAME_LENGTH + 1];

			// Convert UTF-16 partition name to regular string
			char16_str_to_str(entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name_str);

			if (entry_count > 0) {
				nvmeibt_Str_sprintf(json_output, ",\n");
			}
			nvmeibt_Str_sprintf(json_output, "      {\n");
			nvmeibt_Str_sprintf(json_output, "        \"index\": %d,\n", i);
			nvmeibt_Str_sprintf(json_output, "        \"type_guid\": \"%s\",\n", type_urn.str);
			nvmeibt_Str_sprintf(json_output, "        \"partition_guid\": \"%s\",\n", part_urn.str);
			nvmeibt_Str_sprintf(json_output, "        \"pba_s\": %lu,\n", entry->pba_s);
			nvmeibt_Str_sprintf(json_output, "        \"pba_e\": %lu,\n", entry->pba_e);
			nvmeibt_Str_sprintf(json_output, "        \"attributes\": %lu,\n", entry->attributes);
			nvmeibt_Str_sprintf(json_output, "        \"name\": \"%s\"\n", partition_name_str);
			nvmeibt_Str_sprintf(json_output, "      }");
			entry_count++;
		}
	}

	nvmeibt_Str_sprintf(json_output, "\n    ]\n");
	nvmeibt_Str_sprintf(json_output, "  }%s\n", is_last_section ? "" : ",");
}

/**
 * Export GPT to JSON (following nvmeibt_mm_json.c patterns)
 * Uses nvmeibt_Str_sprintf() for JSON serialization
 * Respects --gpt-copy option to export primary, alternate, or both
 */
static int export_gpt_to_json(int disk_fd,
							   struct gpt_util_config *config,
							   const char *output_file)
{
	int										rv = -1;
	struct nvmeibt_Str						*json_output = NULL;
	struct nvmeibt_disk_gpt					temp_gpt;
	struct nvmeibt_disk_gpt					main_gpt_for_metadata;
	struct nvmeibt_disk_mbr					mbr;
	struct gpt_buffers						bufs;
	struct gpt_buffers						metadata_bufs;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_metadata_partition = NULL;
	struct nvmeibt_disk_metadata			*disk_md = NULL;
	enum GPT_VALIDITY						primary_header_validity;
	enum GPT_VALIDITY						alternate_header_validity;
	enum GPT_VALIDITY						primary_entries_validity;
	enum GPT_VALIDITY						alternate_entries_validity;
	enum GPT_VALIDITY						meta_primary_header_validity;
	enum GPT_VALIDITY						meta_alternate_header_validity;
	enum GPT_VALIDITY						meta_primary_entries_validity;
	enum GPT_VALIDITY						meta_alternate_entries_validity;
	time_t									now;
	char									timestamp[64];
	int										output_fd = -1;
	BOOL									is_mismatch = false;
	BOOL									has_overlaps = false;
	BOOL									has_metadata_gpt = false;
	BOOL									has_disk_metadata = false;

	json_output = NNVMEIBT_STR_ALLOC(trace_gpt_json_export);

	// Allocate buffers
	alloc_gpt_buffers(&bufs, config->pblk_size, LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	memset(&temp_gpt, 0, sizeof(temp_gpt));
	temp_gpt.max_n_entries = LARGE_GPT_MAX_NUM_GPT_ENTRIES;
	nvmeibt_strlcpy(temp_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(temp_gpt.main_or_metadata));

	// Read all 4 structures
	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
		NULL, disk_fd, config->pblk_size, &temp_gpt,
		config->pba_s, config->pba_hw_e,
		&primary_header_validity, &alternate_header_validity,
		&primary_entries_validity, &alternate_entries_validity,
		bufs.primary_header, bufs.alternate_header,
		bufs.primary_entries, bufs.alternate_entries);

	// Detect mismatch (only if exporting both copies)
	if (config->gpt_copy_option == GPT_COPY_OPTION_BOTH &&
		primary_header_validity == GPT_VALIDITY_OK && alternate_header_validity == GPT_VALIDITY_OK &&
		primary_entries_validity == GPT_VALIDITY_OK && alternate_entries_validity == GPT_VALIDITY_OK) {
		BOOL is_header_mismatch = !nvmeibt_disk_metadata_are_gpt_headers_equal(bufs.primary_header, bufs.alternate_header);
		BOOL is_entries_mismatch = !nvmeibt_disk_metadata_are_gpt_entries_equal(
			bufs.primary_entries, bufs.alternate_entries,
			bufs.primary_header->n_partition_entries * sizeof(struct nvmeibt_disk_gpt_partition_entry));
		is_mismatch = is_header_mismatch || is_entries_mismatch;
	}

	// Detect overlaps in exported entries (check each bit)
	if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
		has_overlaps = (detect_overlaps(bufs.primary_entries, temp_gpt.max_n_entries) > 0);
	}
	if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		has_overlaps = has_overlaps || (detect_overlaps(bufs.alternate_entries, temp_gpt.max_n_entries) > 0);
	}

	// Get timestamp
	time(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

	// Read pMBR
	memset(&mbr, 0, sizeof(mbr));
	nvmeibt_disk_metadata_read_mbr_blk(NULL, disk_fd, config->pblk_size, &mbr, config->device_path, NULL);

	// Start JSON with metadata
	nvmeibt_Str_sprintf(json_output, "{\n");
	nvmeibt_Str_sprintf(json_output, "  \"backup_timestamp\": \"%s\",\n", timestamp);
	nvmeibt_Str_sprintf(json_output, "  \"device_path\": \"%s\",\n", config->device_path);
	nvmeibt_Str_sprintf(json_output, "  \"=== SECTION 1 ===\": \"AUTO-DETECTED STATUS - DO NOT EDIT\",\n");
	nvmeibt_Str_sprintf(json_output, "  \"_READONLY_mismatch_detected\": %s,\n", is_mismatch ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "  \"_READONLY_overlaps_detected\": %s,\n", has_overlaps ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "  \"=== SECTION 2 ===\": \"DISK STRUCTURE DATA - EDIT WITH CAUTION\",\n");

	memset(&main_gpt_for_metadata, 0, sizeof(main_gpt_for_metadata));
	nvmeibt_strlcpy(main_gpt_for_metadata.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt_for_metadata.main_or_metadata));

	// Check if metadata GPT exists first (determines is_last_section for later exports)
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &main_gpt_for_metadata,
										  config->pba_s, config->pba_hw_e, false) == 0) {
		metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt_for_metadata);
		has_metadata_gpt = (metadata_partition != NULL);
	}

	// Export pMBR
	nvmeibt_Str_sprintf(json_output, "  \"pmbr\": {\n");
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_signature\": \"0x%04x\",\n", mbr.signature);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_os_type\": \"0x%02x\",\n", mbr.partitions[0].os_type);
	nvmeibt_Str_sprintf(json_output, "    \"pba_s\": %d,\n", mbr.partitions[0].pba_s);
	nvmeibt_Str_sprintf(json_output, "    \"n_pblk\": %d\n", mbr.partitions[0].n_pblk);
	nvmeibt_Str_sprintf(json_output, "  }");

	// Export Main GPT based on --gpt-copy option
	nvmeibt_Str_sprintf(json_output, ",\n");
	if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
		BOOL is_last = !(config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) && !has_metadata_gpt;
		export_gpt_copy_entries_to_json(GPT_LEVEL_MAIN, GPT_COPY_PRIMARY,
										bufs.primary_header, bufs.primary_entries,
										temp_gpt.max_n_entries, json_output, is_last);
	}
	if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		BOOL is_last = !has_metadata_gpt;
		export_gpt_copy_entries_to_json(GPT_LEVEL_MAIN, GPT_COPY_ALTERNATE,
										bufs.alternate_header, bufs.alternate_entries,
										temp_gpt.max_n_entries, json_output, is_last);
	}

	// Export Metadata GPT (if exists)
	if (has_metadata_gpt) {
		struct nvmeibt_disk_gpt	metadata_temp_gpt;

		alloc_gpt_buffers(&metadata_bufs, config->pblk_size, MAX_NUM_GPT_ENTRIES);
		memset(&metadata_temp_gpt, 0, sizeof(metadata_temp_gpt));
		metadata_temp_gpt.max_n_entries = MAX_NUM_GPT_ENTRIES;
		nvmeibt_strlcpy(metadata_temp_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_temp_gpt.main_or_metadata));

		// Read all 4 Metadata GPT structures to metadata_bufs
		nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
			NULL, disk_fd, config->pblk_size, &metadata_temp_gpt,
			metadata_partition->pba_s, metadata_partition->pba_e,
			&meta_primary_header_validity, &meta_alternate_header_validity,
			&meta_primary_entries_validity, &meta_alternate_entries_validity,
			metadata_bufs.primary_header, metadata_bufs.alternate_header,
			metadata_bufs.primary_entries, metadata_bufs.alternate_entries);

		// Try to locate EXCELERO_DISK_METADATA partition in the primary entries
		// buffer, and read serial ID/NGUID into disk_md
		for (int k = 0; k < metadata_temp_gpt.max_n_entries; k++) {
			if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_bufs.primary_entries[k])) {
				if (ARE_UUID_EQ(&metadata_bufs.primary_entries[k].partition_type_guid,
								&EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID)) {
					disk_metadata_partition = &metadata_bufs.primary_entries[k];
					break;
				}
			}
		}
		if (disk_metadata_partition) {
			uint64_t pbyte_s = disk_metadata_partition->pba_s * config->pblk_size;
			disk_md = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_export_disk_md, PAGE_SIZE, sizeof(*disk_md));
			if (nvmeibt_disk_metadata_read_disk_metadata(NULL, disk_fd, config->pblk_size,
														 pbyte_s, disk_md) == 0) {
				has_disk_metadata = true;
			}
		}

		// Export Metadata GPT
		if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
			BOOL is_last = !(config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) && !has_disk_metadata;
			export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_PRIMARY,
											metadata_bufs.primary_header,
											metadata_bufs.primary_entries,
											metadata_temp_gpt.max_n_entries, json_output, is_last);
		}
		if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
			BOOL is_last = !has_disk_metadata;
			export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_ALTERNATE,
											metadata_bufs.alternate_header,
											metadata_bufs.alternate_entries,
											metadata_temp_gpt.max_n_entries, json_output, is_last);
		}

		// Export disk_metadata (if available) - modifiable on apply
		if (has_disk_metadata) {
			struct nvmeibt_urn_uuid mgmt_uuid_urn;
			struct nvmeibt_urn_uuid nguid_urn;

			N_Tf(gpt_export_disk_md, "Exporting disk_metadata structure");

			nvmeibt_Str_sprintf(json_output, "  \"disk_metadata\": {\n");

			// Static field (do not edit)
			nvmeibt_Str_sprintf(json_output, "    \"_STATIC_signature\": \"0x%lx\",\n", disk_md->signature);

			// Editable fields
			nvmeibt_Str_sprintf(json_output, "    \"last_pba_zeroed\": %lu,\n", disk_md->last_pba_zeroed);

			mgmt_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&disk_md->mgmt_db_uuid);
			nvmeibt_Str_sprintf(json_output, "    \"mgmt_db_uuid\": \"%s\",\n", mgmt_uuid_urn.str);
			nvmeibt_Str_sprintf(json_output, "    \"disk_metadata_version\": %u,\n", disk_md->disk_metadata_version);
			nvmeibt_Str_sprintf(json_output, "    \"format_pblk_size\": %u,\n", disk_md->format_pblk_size);
			nvmeibt_Str_sprintf(json_output, "    \"format_metadata_size\": %u,\n", disk_md->format_metadata_size);
			nvmeibt_Str_sprintf(json_output, "    \"format_request_counter\": %u,\n", disk_md->format_request_counter);
			nvmeibt_Str_sprintf(json_output, "    \"ldisk_id_str\": \"%s\",\n", disk_md->ldisk_id_str);
			nvmeibt_Str_sprintf(json_output, "    \"nsid\": %d,\n", disk_md->nsid);
			nvmeibt_Str_sprintf(json_output, "    \"native_serial_str\": \"%s\",\n", disk_md->native_serial_str);

			nguid_urn = nvmeibt_union_uuid_to_urn_uuid(&disk_md->native_nguid_unused);
			nvmeibt_Str_sprintf(json_output, "    \"native_nguid\": \"%s\",\n", nguid_urn.str);

			// Read-only field (recalculated on write)
			nvmeibt_Str_sprintf(json_output, "    \"_READONLY_crc32\": \"0x%08x\"\n", disk_md->crc32);
			nvmeibt_Str_sprintf(json_output, "  }");
		}

		free_gpt_buffers(&metadata_bufs);
		NNVMEIBT_BM_FREE(trace_gpt_export_disk_md_free, disk_md);
	}

	nvmeibt_Str_sprintf(json_output, "\n}\n");

	// Write JSON to file
	output_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd < 0) {
		N_Ef(gpt_json_create_failed, "Failed to create JSON output file @STR @AUTO_ERRNO", output_file);
		goto out;
	}

	if (write(output_fd, nvmeibt_Str_str(json_output), nvmeibt_Str_strlen(json_output)) < 0) {
		N_Ef(gpt_json_write_failed, "Failed to write JSON to file @STR @AUTO_ERRNO", output_file);
		goto out;
	}

	N_IMf(gpt_json_export_success, "GPT exported to JSON: dev=@STR file=@STR bytes=@SIZE_T copy_option=@STR has_metadata_gpt=@INT has_disk_md=@INT",
		  config->device_path, output_file, nvmeibt_Str_strlen(json_output), gpt_copy_option_str(config->gpt_copy_option), has_metadata_gpt, has_disk_metadata);
	fprintf(stdout, COL_GREEN "GPT exported to JSON: %s (%lu bytes)" COL_RESET "\n", output_file, nvmeibt_Str_strlen(json_output));

	// Build status message
	fprintf(stdout, "  - Exported: pMBR, Main GPT");
	if (has_metadata_gpt) {
		fprintf(stdout, ", Metadata GPT");
		if (has_disk_metadata) {
			fprintf(stdout, ", disk_metadata");
		}
	}
	fprintf(stdout, "\n");

	// Warn if mismatch or overlaps detected
	if (is_mismatch) {
		N_Wf(gpt_json_mismatch_detected, "Mismatch detected in exported GPT: dev=@STR file=@STR",
			 config->device_path, output_file);
		fprintf(stdout, "\n");
		fprintf(stdout, COL_YELLOW "*** WARNING: Primary and alternate copies differ! ***" COL_RESET "\n");
		fprintf(stdout, "    JSON marked with '_READONLY_mismatch_detected: true'\n");
		fprintf(stdout, "    Apply will be BLOCKED until you choose one copy.\n");
		fprintf(stdout, "    " COL_GREEN "Suggestion: Re-export with --gpt-copy=primary or --gpt-copy=alternate" COL_RESET "\n");
	}
	if (has_overlaps) {
		N_Wf(gpt_json_overlaps_detected, "Overlapping partitions detected in exported GPT: dev=@STR file=@STR",
			 config->device_path, output_file);
		fprintf(stdout, "\n");
		fprintf(stdout, COL_YELLOW "*** WARNING: Overlapping partitions detected! ***" COL_RESET "\n");
		fprintf(stdout, "    JSON marked with '_READONLY_overlaps_detected: true'\n");
		fprintf(stdout, "    Apply will be BLOCKED until overlaps are fixed.\n");
	}

	rv = 0;

out:
	if (output_fd >= 0) {
		close(output_fd);
	}
	free_gpt_buffers(&bufs);
	NNVMEIBT_STR_FREE(trace_gpt_json_export_free, json_output);
	return rv;
}

int get_device_info(const char* dev_name, int* pblk_size, uint64_t* pba_e)
{
	int			fd = -1;
	int			rv = -1;
	uint64_t	n_bytes_dev;
	int			pblk_size_dev;
	struct stat	st;

	rv = NNVMEIBT_OPEN_READ(trace_gpt_util_get_device_info, dev_name, 1);
	if (rv < 0) {
		N_Ef(gpt_util_open_failed, "Failed opening device=@STR @AUTO_ERRNO", dev_name);
		goto out;
	}
	fd = rv;

	// Detect file type first
	rv = fstat(fd, &st);
	if (rv < 0) {
		N_Ef(gpt_util_fstat_failed, "Failed to stat=@STR @AUTO_ERRNO", dev_name);
		goto out;
	}

	if (S_ISREG(st.st_mode)) {
		// Regular file - use fstat for size
		pblk_size_dev = 4096;		// Default block size for regular files
		n_bytes_dev = st.st_size;
		fprintf(stdout, "Regular file detected: %s (size=%lu bytes)\n", dev_name, n_bytes_dev);
	} else if (S_ISBLK(st.st_mode)) {
		// Block device - use ioctl
		rv = ioctl(fd, BLKSSZGET, &pblk_size_dev);
		if (rv < 0) {
			N_Ef(gpt_util_ioctl_blksz_failed, "Failed fetching block size=@STR @AUTO_ERRNO", dev_name);
			goto out;
		}

		rv = ioctl(fd, BLKGETSIZE64, &n_bytes_dev);
		if (rv < 0) {
			N_Ef(gpt_util_ioctl_size_failed, "Failed fetching disk size=@STR @AUTO_ERRNO", dev_name);
			goto out;
		}
	} else {
		N_Ef(gpt_util_unsupported_file_type, "Unsupported file type for @STR", dev_name);
		rv = -1;
		goto out;
	}

	if (pblk_size) {
		*pblk_size = pblk_size_dev;
	}
	if (pba_e) {
		*pba_e = (n_bytes_dev - 1) / pblk_size_dev;
	}

out:
	NNVMEIBT_CLOSE(trace_1_gpt_util_get_device_info, fd);
	return rv;
}

/**
 * Generate a mock NVMesh disk with valid MBR and GPT structure for self-test
 * Returns the fd of the created device (caller must close it)
 * In sandbox: file auto-deleted when fd closed (O_CREAT tracked)
 * In production: caller should unlink file when done
 */

static void print_usage(char *argv[])
{
	fprintf(stdout, "Usage: %s [OPTIONS]\n\n", argv[0]);

	fprintf(stdout, "Device Context:\n");
	fprintf(stdout, "  -d, --device=PATH           NVMesh managed device (reads disks csv)\n");
	fprintf(stdout, "  -a, --any-device=PATH       Any block device or regular file\n");
	fprintf(stdout, "  -s, --pba-s=NUM             Override start PBA\n");
	fprintf(stdout, "  -e, --pba-e=NUM             Override end PBA\n");
	fprintf(stdout, "  -b, --block-size=SIZE       Override block size\n\n");

	fprintf(stdout, "Actions (choose one, default is display GPT):\n");
	fprintf(stdout, "  -m, --print-mbr             Display MBR only\n");
	fprintf(stdout, "  -i, --check-excelero        Check if EXCELERO_METADATA partition exists\n");
	fprintf(stdout, "  -f, --fix-gpt               Fix GPT from alternate copy (and display)\n");
	fprintf(stdout, "  -F, --fix-mbr               Fix MBR (and display)\n");
	fprintf(stdout, "  -U, --upgrade-gpt           Fix n_partition_entries to 8192 and recalculate CRC\n");
	fprintf(stdout, "  --output-json=FILE          Export GPT to JSON file\n");
	fprintf(stdout, "  --apply-from=FILE           Apply GPT from JSON file (dry-run by default)\n");
	fprintf(stdout, "  (default: display GPT)      Display GPT structure\n\n");

	fprintf(stdout, "Display Options:\n");
	fprintf(stdout, "  -c, --gpt-copy=WHICH        Which copy: primary|alternate|both (default: primary)\n");
	fprintf(stdout, "  --filter-uuid=UUID          Show only entries matching UUID\n");
	fprintf(stdout, "  --filter-lba=ADDR           Show only entries containing LBA address\n");
	fprintf(stdout, "  -Z, --print-zero-verify     Print commands that verify zeroed ranges\n\n");

	fprintf(stdout, "JSON Export Options:\n");
	fprintf(stdout, "  --output-json=FILE          Export GPT to JSON\n\n");

	fprintf(stdout, "Apply Options:\n");
	fprintf(stdout, "  --write                     Actually write changes (default: dry-run)\n\n");

	fprintf(stdout, "I/O Options:\n");
	fprintf(stdout, "  --direct                    Force O_DIRECT even for regular files (may fail)\n");
	fprintf(stdout, "  --no-direct                 Disable O_DIRECT even for block devices\n");
	fprintf(stdout, "  (default: auto)             Block devices use O_DIRECT, files don't\n\n");

	fprintf(stdout, "Testing:\n");
	fprintf(stdout, "  -T, --self-test             Run comprehensive self-test suite\n");
}


/**
 * Phase 1: Parse command-line arguments into config structure
 * Returns 0 on success, -1 on error
 */
static int parse_arguments(int argc, char *argv[], struct gpt_util_config *config)
{
	int								rv = 0;
	int								op;
	long							i;
	char							*_argv[argc];
	struct nvmeibt_Str				*new_config = NULL;
	static struct option long_options[] =
	{
		{"device",					required_argument,	0,	'd'},
		{"any-device",				required_argument,	0,	'a'},
		{"pba-s",					required_argument,	0,	's'},
		{"pba-e",					required_argument,	0,	'e'},
		{"block-size",				required_argument,	0,	'b'},
		{"check-excelero",			no_argument,		0,	'i'},
		{"print-mbr",				no_argument,		0,	'm'},
		{"fix-gpt",					no_argument,		0,	'f'},
		{"fix-mbr",					no_argument,		0,	'F'},
		{"upgrade-gpt",				no_argument,		0,	'U'},
		{"gpt-copy",				required_argument,	0,	'c'},
		{"filter-uuid",				required_argument,	0,	'u'},
		{"filter-lba",				required_argument,	0,	'l'},
		{"print-zero-verify",		no_argument,		0,	'Z'},
		{"output-json",				required_argument,	0,	'J'},
		{"apply-from",				required_argument,	0,	'A'},
		{"write",					no_argument,		0,	'W'},
		{"direct",					no_argument,		0,	'D'},
		{"no-direct",				no_argument,		0,	'N'},

		{0, 0, 0, 0}
	};
	static const char short_options[] = "d:a:s:e:b:c:u:l:J:A:ZimfFUWDN";
	static int long_idx = -1;

	for (i = 0; i < argc; ++i) {
		_argv[i] = trim_whitespace(argv[i]);
	}

	new_config = NNVMEIBT_STR_ALLOC(trace_parse_args_config);
	while ((op = getopt_long(argc, _argv, short_options, long_options, &long_idx)) != -1) {
		const char				*csv_str_end;
		const char				*scan_line_ptr;
		char					line[NVMEIBT_MAX_CSV_LINE_LENGTH];
		int						line_len;
		int						is_expecting_csv_header_line;
		enum NVMEIBT_CSV_TYPE	section_type;

		if (optarg) {
			optarg = trim_whitespace(optarg);
		}
		switch (op) {
		case 'd':
			nvmeibt_strlcpy(config->device_path, optarg, sizeof(config->device_path));
			config->is_nvmesh_managed = true;
			fprintf(stdout, "Device: %s (NVMesh managed)\n", config->device_path);
			// Read disks file data
			nvmeibt_Str_reuse(new_config);
			rv = nvmeib_srvr_api_lib_get_csv_disks(new_config);
			if (rv < 0) {
				N_Ef(t_zzz_26, "Error getting disks csv, @AUTO_ERRNO");
				rv = -1;
				goto out;
			}

			// Parse disks csv to find device parameters
			csv_str_end = nvmeibt_Str_str(new_config) + nvmeibt_Str_strlen(new_config);
			scan_line_ptr = nvmeibt_Str_str(new_config);
			is_expecting_csv_header_line = 1;
			section_type = NVMEIBT_CSV_TYPE_LOCAL_DISKS;
			while (scan_line_ptr < csv_str_end - 1) {
				const char *scan_line_end = (char *)memchr(scan_line_ptr, '\n', csv_str_end - scan_line_ptr);
				if (!scan_line_end) {
					fprintf(stdout,"Missing \\n at the end of csv line '%s'\n", scan_line_ptr);
					scan_line_end = csv_str_end;
				}
				line_len = strnlen(scan_line_ptr, scan_line_end - scan_line_ptr);
				if (line_len >= (int)sizeof(line))
					continue;
				memcpy(line, scan_line_ptr, line_len);
				scan_line_ptr = scan_line_ptr + line_len + 1;
				line[line_len] = '\0';
				fprintf(stdout,"read line of len=%d, '%s'\n", line_len, line);
				if (line_len < 1) {
					continue;
				}

				fprintf(stdout,"is_expecting_csv_header_line=%d\n", is_expecting_csv_header_line);
				if (is_expecting_csv_header_line) {
					const char *ref_header = nvmeibt_get_csv_header_by_section_type(section_type);
					if (ref_header == NULL) {
						N_Ef(parse_csv_bad_section_type, "Wrong section_type=@INT", section_type);
						rv = -1;
						goto out;
					}
					if (memcmp(line, ref_header, line_len) != 0) {
						N_Ef(parse_csv_header_mismatch, "Expecting csv header '@STR'. Got '@STR'", ref_header, line);
						rv = -1;
						goto out;
					}
					fprintf(stdout,"The header csv header line:%s\n", ref_header);
					is_expecting_csv_header_line = 0;
				} else {
					int r;
					struct nvmeibt_local_disk_config disk_config;

					memset(&disk_config, 0, sizeof(disk_config));
					r = nvmeibt_sscanf_csv_line(line,
						's', sizeof(disk_config.ldisk_id.str), &disk_config.ldisk_id.str,
						'D', &disk_config.n_pblk,
						'D', &disk_config.n_hw_pblk,
						'd', &disk_config.pblk_size,
						'd', &disk_config.max_request_size,
						'd', &disk_config.seq,
						'd', &disk_config.nsid,
						's', sizeof(disk_config.dev_file_name), &disk_config.dev_file_name,
						'd', &disk_config.metadata_n_bytes,
						's', sizeof(disk_config.status), &disk_config.status,
						'd', &disk_config.vendor,
						's', sizeof(disk_config.smart_info.Model), &disk_config.smart_info.Model,
						's', sizeof(disk_config.native_serial.str), &disk_config.native_serial.str,
						'\0');
					if (r < 0) {
						N_Wf(parse_csv_line_failed, "Failed to parse CSV line: @STR", line);
					}

					// Check if this is the device we are looking for
					if (strncmp(config->device_path, disk_config.dev_file_name,
								min((size_t)NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN, sizeof(config->device_path) - 1)) == 0) {
						// Found the disk - take its parameters
						config->pba_s = 1;
						config->pba_e = disk_config.n_pblk - 1;
						config->pba_hw_e = disk_config.n_hw_pblk - 1;
						config->pblk_size = disk_config.pblk_size;
						fprintf(stdout,"Found device=%s in disks csv, using pba_s=0x%lx, pba_e=0x%lx, block_size=%d\n",
								config->device_path, config->pba_s, config->pba_e, config->pblk_size);
						break;
					} else {
						fprintf(stdout, "Device=%s is different from requested=%s skipping\n",
								disk_config.dev_file_name, config->device_path);
					}
				}
			}
			break;
		case 's':
			config->pba_s = (uint64_t)atoi(optarg);
			fprintf(stdout, "Override pba_s=0x%lx\n", config->pba_s);
			break;
		case 'e':
			config->pba_e = (uint64_t)atoi(optarg);
			fprintf(stdout, "Override pba_e=0x%lx\n", config->pba_e);
			break;
		case 'b':
			config->pblk_size = atoi(optarg);
			fprintf(stdout, "Override block_size=%d\n", config->pblk_size);
			break;
		case 'm':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_DISPLAY_MBR;
			fprintf(stdout, "Action: Display MBR\n");
			break;
		case 'f':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_fix_gpt, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_FIX_GPT;
			fprintf(stdout, "Action: Fix GPT from alternate copy\n");
			break;
		case 'F':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_fix_mbr, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_FIX_MBR;
			fprintf(stdout, "Action: Fix MBR\n");
			break;
		case 'i':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_check, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_CHECK_EXCELERO;
			fprintf(stdout, "Action: Check for EXCELERO_METADATA partition\n");
			break;
		case 'U':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_upgrade, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_UPGRADE_GPT;
			fprintf(stdout, "Action: Fix n_partition_entries to %d and recalculate CRC\n",
					LARGE_GPT_MAX_NUM_GPT_ENTRIES);
			break;
		case 'c':
			if (strcmp(optarg, "primary") == 0) {
				config->gpt_copy_option = GPT_COPY_OPTION_PRIMARY;
			} else if (strcmp(optarg, "alternate") == 0) {
				config->gpt_copy_option = GPT_COPY_OPTION_ALTERNATE;
			} else if (strcmp(optarg, "both") == 0) {
				config->gpt_copy_option = GPT_COPY_OPTION_BOTH;
			} else {
				N_Ef(parse_invalid_gpt_copy, "Invalid --gpt-copy value @STR (use: primary|alternate|both)", optarg);
				rv = -1;
				goto out;
			}
			fprintf(stdout, "GPT copy selection: %s\n", gpt_copy_option_str(config->gpt_copy_option));
			break;
		case 'u':
			nvmeibt_strlcpy(config->filter_uuid_str, optarg, sizeof(config->filter_uuid_str));
			config->has_uuid_filter = true;
			fprintf(stdout, "Filter by UUID: %s\n", config->filter_uuid_str);
			break;
		case 'l':
			config->filter_lba = (uint64_t)atoll(optarg);
			config->has_lba_filter = true;
			fprintf(stdout, "Filter by LBA: 0x%lx\n", config->filter_lba);
			break;
		case 'Z':
			config->print_zero_verify_cmds = true;
			fprintf(stdout, "Print zeroing verification commands: ENABLED\n");
			break;
		case 'J':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_export, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_EXPORT_JSON;
			nvmeibt_strlcpy(config->output_json_file, optarg, sizeof(config->output_json_file));
			fprintf(stdout, "Action: Export GPT to JSON file: %s\n", config->output_json_file);
			break;
		case 'A':
			if (config->action != ACTION_DISPLAY_GPT) {
				N_Ef(parse_multiple_actions_apply, "Multiple actions specified (only one allowed)");
				rv = -1;
				goto out;
			}
			config->action = ACTION_APPLY_JSON;
			nvmeibt_strlcpy(config->apply_json_file, optarg, sizeof(config->apply_json_file));
			fprintf(stdout, "Action: Apply GPT from JSON file: %s (dry-run by default)\n", config->apply_json_file);
			break;
		case 'W':
			config->write_mode = true;
			fprintf(stdout, "Write mode: ENABLED (changes will be written to disk)\n");
			break;
		case 'D':
			config->o_direct_mode = O_DIRECT_FORCE_ON;
			fprintf(stdout, "I/O mode: Force O_DIRECT (may fail for regular files)\n");
			break;
		case 'N':
			config->o_direct_mode = O_DIRECT_FORCE_OFF;
			fprintf(stdout, "I/O mode: Disable O_DIRECT\n");
			break;
		case 'a':
			nvmeibt_strlcpy(config->device_path, optarg, sizeof(config->device_path));
			config->is_nvmesh_managed = false;
			fprintf(stdout, "Device: %s (any device)\n", config->device_path);
			// Get device info immediately to fill config
			if (get_device_info(config->device_path, &config->pblk_size, &config->pba_e) < 0) {
				N_Ef(parse_get_device_info_failed, "Failed to get device info for @STR", config->device_path);
				rv = -1;
				goto out;
			}
			config->pba_s = 1;
			config->pba_hw_e = config->pba_e;
			break;
		default:
			N_Ef(parse_unknown_option, "Unknown command-line option");
			print_usage(argv);
			rv = -1;
			goto out;
		}
	}

	rv = 0;

out:
	NNVMEIBT_STR_FREE(trace_parse_args_cleanup, new_config);
	return rv;
}

/**
 * Phase 2: Validate configuration
 * Returns 0 if valid, -1 on error
 */
static int validate_config(struct gpt_util_config *config)
{
	// Must specify device
	if (config->device_path[0] == '\0') {
		N_Ef(validate_no_device, "Must specify device with -d or -a");
		return -1;
	}

	return 0;
}

/**
 * Phase 3: Execute CHECK_EXCELERO action
 */
static int execute_check_excelero(int disk_fd, struct gpt_util_config *config)
{
	int						rv = -1;
	struct nvmeibt_disk_gpt	temp_gpt;

	memset(&temp_gpt, 0, sizeof(temp_gpt));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &temp_gpt,
										  config->pba_s, config->pba_hw_e, false) == 0) {
		if (nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&temp_gpt)) {
			fprintf(stdout, "%s EXCELERO_METADATA_FOUND\n", config->device_path);
			rv = 0;
		} else {
			fprintf(stdout, "%s EXCELERO_METADATA_NOT_FOUND\n", config->device_path);
			rv = 0;
		}
	} else {
		N_Ef(check_excelero_read_failed, "Failed to read GPT from device @STR", config->device_path);
		rv = -1;
	}

	return rv;
}

/**
 * Phase 3: Execute DISPLAY_MBR action
 */
static int execute_display_mbr(int disk_fd, struct gpt_util_config *config)
{
	int						rv = -1;
	struct nvmeibt_disk_mbr	mbr;
	struct nvmeibt_Str		*outstr;

	memset(&mbr, 0, sizeof(mbr));

	if (nvmeibt_disk_metadata_read_mbr_blk(NULL, disk_fd, config->pblk_size, &mbr,
										   config->device_path, NULL) < 0) {
		N_Ef(display_mbr_read_failed, "Unable to read MBR dev=@STR block_size=@INT",
			 config->device_path, config->pblk_size);
		goto out;
	}

	outstr = NNVMEIBT_STR_ALLOC(trace_display_mbr);
	nvmeibt_disk_metadata_fill_dump_mbr_str(outstr, &mbr);
	fprintf(stdout, "%s\n", nvmeibt_Str_str(outstr));
	NNVMEIBT_STR_FREE(trace_display_mbr_free, outstr);

	rv = 0;

out:
	return rv;
}

/**
 * Phase 3: Execute FIX_MBR action
 */
static int execute_fix_mbr(int disk_fd, struct gpt_util_config *config)
{
	int						rv = -1;
	struct nvmeibt_disk_mbr	mbr;
	struct nvmeibt_Str		*outstr;

	memset(&mbr, 0, sizeof(mbr));

	// Read current MBR
	if (nvmeibt_disk_metadata_read_mbr_blk(NULL, disk_fd, config->pblk_size, &mbr,
										   config->device_path, NULL) < 0) {
		N_Ef(fix_mbr_read_failed, "Unable to read MBR dev=@STR block_size=@INT",
			 config->device_path, config->pblk_size);
		goto out;
	}

	// Display current state
	outstr = NNVMEIBT_STR_ALLOC(trace_fix_mbr_before);
	nvmeibt_disk_metadata_fill_dump_mbr_str(outstr, &mbr);
	fprintf(stdout, "Current MBR:\n%s\n", nvmeibt_Str_str(outstr));
	NNVMEIBT_STR_FREE(trace_fix_mbr_before_free, outstr);

	// Check if fix needed
	if (mbr.signature != (short)MBR_SIGNATURE) {
		fprintf(stdout, "MBR signature invalid, attempting fix...\n");
		if (config->pba_e == 0) {
			N_Ef(fix_mbr_no_size, "Disk size not detected, please specify with -e");
			goto out;
		}

		N_IMf(fix_mbr_before, "Fixing MBR on dev=@STR old_signature=@X new_signature=@X pba_e=@ZX",
			  config->device_path, mbr.signature, (short)MBR_SIGNATURE, config->pba_e);

		nvmeibt_disk_metadata_init_pmbr(&mbr, config->pba_e + 1, config->pblk_size);
		if (nvmeibt_disk_metadata_write_mbr(NULL, disk_fd, config->pblk_size, &mbr) < 0) {
			N_Ef(fix_mbr_write_failed, "MBR write failed for dev=@STR", config->device_path);
			goto out;
		}
		N_IMf(fix_mbr_success, "MBR fix successful on dev=@STR", config->device_path);
		fprintf(stdout, "MBR fix successful\n");
	} else {
		N_Tf(fix_mbr_already_valid, "MBR is valid on dev=@STR (no fix needed)", config->device_path);
		fprintf(stdout, "MBR is valid, no fix needed\n");
	}

	rv = 0;

out:
	return rv;
}

/**
 * Phase 3: Execute FIX_GPT action
 */
static int execute_fix_gpt(int disk_fd, struct gpt_util_config *config)
{
	int		rv;

	N_IMf(fix_gpt_start, "Fixing GPT from another copy: dev=@STR pba_s=@ZX pba_hw_e=@ZX",
		  config->device_path, config->pba_s, config->pba_hw_e);

	// display_all_gpts() with fix_gpt=true will fix from alternate and display
	rv = display_all_gpts(disk_fd, config->pblk_size, config->pba_s, config->pba_hw_e,
						  config->gpt_copy_option, config->device_path, true, config);

	if (rv == 0) {
		N_IMf(fix_gpt_success, "GPT fix complete: dev=@STR", config->device_path);
	}

	return rv;
}

/**
 * Upgrade GPT in place: check if needed, fix n_partition_entries, recalculate CRC
 * Uses TOMA API nvmeibt_disk_metadata_store_gpt which internally calls update_gpt_crcs()
 * Returns: 0 on success (upgraded or no upgrade needed), -1 on error
 */
static int upgrade_gpt_if_needed(int disk_fd, int pblk_size, struct nvmeibt_disk_gpt *gpt, const char *gpt_name)
{
	uint32_t	expected_crc;
	int			nbytes;
	int			old_n_partition_entries;
	uint32_t	old_entries_crc = gpt->header.partition_entry_array_crc32;
	uint32_t	old_header_crc = gpt->header.header_crc32;

	// Only upgrade LARGE GPTs
	if (gpt->max_n_entries != LARGE_GPT_MAX_NUM_GPT_ENTRIES) {
		fprintf(stdout, "%s GPT: max_n_entries=%d (not LARGE GPT, skipping)\n",
				gpt_name, gpt->max_n_entries);
		return 0;
	}

	// Validate CRC. For LARGE GPT, CRC always uses 8192 entries.
	nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * gpt->header.size_of_partition_entry;
	expected_crc = crc32_seedless(gpt->entries, nbytes);
	if (old_entries_crc != expected_crc) {
		N_Ef(upgrade_gpt_crc_corrupt, "@STR GPT: CRC invalid (corruption?). Will not upgrade.", gpt_name);
		return -1;
	}

	// Already correct?
	if (gpt->header.n_partition_entries == LARGE_GPT_MAX_NUM_GPT_ENTRIES) {
		fprintf(stdout, "%s GPT: Already correct (n_partition_entries=%d)\n",
				gpt_name, LARGE_GPT_MAX_NUM_GPT_ENTRIES);
		return 0;
	}

	// TODO(NVMESH-7436): Remove this block once ticket is fixed.
	if (1) {
		fprintf(stdout, COL_YELLOW "%s GPT: Blocking gpt upgrade (n_partition_entries remains %d), as it may prevent some machines from booting." COL_RESET "\n",
			gpt_name, gpt->header.n_partition_entries);
		return 0;
	}

	// Perform upgrade
	old_n_partition_entries = gpt->header.n_partition_entries;
	gpt->header.n_partition_entries = LARGE_GPT_MAX_NUM_GPT_ENTRIES;

	N_IMf(upgrade_gpt_before, "Upgrading @STR GPT: n_part @INT -> @INT disk_uuid=@UUID_LE CRC_before: hdr=@CRC ent=@CRC",
		  gpt_name, old_n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES, &gpt->header.disk_obj_uuid,
		  old_header_crc, old_entries_crc);
	fprintf(stdout, "%s GPT: Upgrading (n_partition_entries: %d -> %d)\n",
			gpt_name, old_n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	if (nvmeibt_disk_metadata_store_gpt(NULL, disk_fd, pblk_size, gpt, false) < 0) {
		N_Ef(upgrade_gpt_write_failed, "Failed to write @STR GPT", gpt_name);
		return -1;
	}

	// Log new CRCs (store_gpt recalculates them)
	N_IMf(upgrade_gpt_success, "@STR GPT upgrade complete", gpt_name);
	fprintf(stdout, "%s GPT: Upgrade complete\n", gpt_name);
	return 0;
}

/**
 * Phase 3: Execute UPGRADE_GPT action
 * Fixes n_partition_entries to LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192) and recalculates CRC
 */
static int execute_upgrade_gpt(int disk_fd, struct gpt_util_config *config)
{
	int										rv = -1;
	struct nvmeibt_disk_gpt					main_gpt;
	struct nvmeibt_disk_gpt					metadata_gpt;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_entry;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));

	fprintf(stdout, "\n=== GPT Upgrade Check (n_partition_entries -> %d) ===\n\n", LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	// Step 1: Read Main GPT (with backward compatibility validation)
	fprintf(stdout, "Reading Main GPT...\n");
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &main_gpt,
										  config->pba_s, config->pba_hw_e, false) < 0) {
		N_Ef(upgrade_gpt_read_main_failed, "Failed to read Main GPT dev=@STR. GPT may be corrupted.",
			 config->device_path);
		fprintf(stdout, "Use --fix-gpt first to recover from alternate copy.\n");
		goto out;
	}
	fprintf(stdout, "Main GPT read successfully (n_partition_entries=%d, max_n_entries=%d)\n",
			main_gpt.header.n_partition_entries, main_gpt.max_n_entries);

	// Step 2: Upgrade Main GPT if needed
	if (upgrade_gpt_if_needed(disk_fd, config->pblk_size, &main_gpt, "Main") < 0) {
		goto out;
	}

	// Step 3: Check for Metadata GPT
	metadata_entry = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_entry) {
		fprintf(stdout, "\nNo Metadata GPT found (non-NVMesh disk or no excelero_metadata partition)\n");
		goto out;
	}
	fprintf(stdout, "\nReading Metadata GPT...\n");
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &metadata_gpt, metadata_entry->pba_s, metadata_entry->pba_e, false) < 0) {
		N_Wf(upgrade_gpt_read_metadata_failed, "Failed to read Metadata GPT dev=@STR. Skipping metadata upgrade.", config->device_path);
		goto out;
	}
	fprintf(stdout, "Metadata GPT read successfully (n_partition_entries=%d, max_n_entries=%d)\n",
			metadata_gpt.header.n_partition_entries, metadata_gpt.max_n_entries);

	// Upgrade Metadata GPT if needed
	if (upgrade_gpt_if_needed(disk_fd, config->pblk_size, &metadata_gpt, "Metadata") < 0) {
		N_Ef(upgrade_metadata_gpt_failed, "Failed to upgrade Metadata GPT dev=@STR", config->device_path);
		// Don't fail entire operation if only metadata upgrade fails
	}

	fprintf(stdout, "\n=== GPT Upgrade Complete ===\n");
	rv = 0;

out:
	return rv;
}

/**
 * Modify JSON string field (SELF-TEST helper)
 * Parses JSON, modifies field using base library, serializes back
 * Returns 0 on success, -1 on error
 */

/**
 * Parse one GPT entry from JSON dict element
 * Fills in the provided entry structure
 * Returns 0 on success, -1 on error
 */
static int parse_gpt_entry_from_json(struct nvmeibt_disk_gpt_partition_entry *entry,
									  struct mm_json_elem *entry_elem)
{
	int							rv = -1;
	struct mm_json_kv_pair		*kv = NULL;
	struct mm_json_dict			*dict = NULL;
	char						*type_guid_str = NULL;
	char						*partition_guid_str = NULL;
	char						*name_str = NULL;
	JSON_ASSIGN_AND_CALL_INIT();

	if (!entry_elem || entry_elem->type != JSON_E_DICT) {
		N_Ef(parse_entry_not_dict, "Entry element is not a dict type=@INT", entry_elem ? (int)entry_elem->type : -1);
		return -1;
	}

	memset(entry, 0, sizeof(*entry));

	dict = &entry_elem->dict;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(parse_entry, kv->key);
		JSON_ASSIGN_PLAIN(parse_type_guid, "type_guid", type_guid_str, kv->value->str);
		JSON_ASSIGN_PLAIN(parse_part_guid, "partition_guid", partition_guid_str, kv->value->str);
		JSON_ASSIGN_PLAIN(parse_pba_s, "pba_s", entry->pba_s, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_pba_e, "pba_e", entry->pba_e, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_attr, "attributes", entry->attributes, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_name, "name", name_str, kv->value->str);
		JSON_ASSIGN_OPTIONAL(parse_index, "index");		// Optional, just for display
		JSON_LOOP_ITERATION_END(parse_entry_end, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(parse_entry_validate);

	// Convert UUIDs from strings
	if (type_guid_str) {
		nvmeibt_urn_uuid_str_to_union_uuid(&entry->partition_type_guid, type_guid_str);
	}
	if (partition_guid_str) {
		nvmeibt_urn_uuid_str_to_union_uuid(&entry->partition_guid, partition_guid_str);
	}
	if (name_str) {
		str_to_char16_str(name_str, strlen(name_str), entry->partition_name);
	}

	rv = 0;
	return rv;
}

/**
 * Parse GPT from JSON section (main_gpt_primary or main_gpt_alternate)
 * Builds a complete nvmeibt_disk_gpt structure from JSON
 * Caller must allocate gpt->entries buffer before calling
 * Returns 0 on success, -1 on error
 */
static int parse_gpt_from_json_section(struct nvmeibt_disk_gpt *gpt,
										struct mm_json_elem *gpt_section_elem,
										const char *section_name)
{
	int							rv = -1;
	int							i;
	int							j;
	struct mm_json_kv_pair		*kv = NULL;
	struct mm_json_dict			*dict = NULL;
	struct mm_json_elem			*entries_array = NULL;
	int							n_entries_in_json = 0;
	char						*disk_uuid_str = NULL;
	JSON_ASSIGN_AND_CALL_INIT();

	if (!gpt_section_elem || gpt_section_elem->type != JSON_E_DICT) {
		N_Ef(parse_gpt_not_dict, "GPT section @STR is not a dict", section_name);
		return -1;
	}

	// Extract fields from GPT section using JSON macros
	dict = &gpt_section_elem->dict;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(parse_gpt_sec, kv->key);
		JSON_ASSIGN_PLAIN(parse_disk_uuid, "disk_uuid", disk_uuid_str, kv->value->str);
		JSON_ASSIGN_PLAIN(parse_n_part, "n_partition_entries", gpt->header.n_partition_entries, (uint32_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_first_pba, "first_usable_pba", gpt->header.first_usable_pba, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_last_pba, "last_usable_pba", gpt->header.last_usable_pba, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_entries_arr, "entries", entries_array, kv->value);
		JSON_ASSIGN_OPTIONAL(parse_static_sig, "_STATIC_gpt_signature");
		JSON_ASSIGN_OPTIONAL(parse_static_rev, "_STATIC_revision");
		JSON_ASSIGN_OPTIONAL(parse_static_hdr_sz, "_STATIC_header_size");
		JSON_ASSIGN_OPTIONAL(parse_static_ent_sz, "_STATIC_size_of_partition_entry");
		JSON_ASSIGN_OPTIONAL(parse_ro_hdr_crc, "_READONLY_header_crc32");
		JSON_ASSIGN_OPTIONAL(parse_ro_ent_crc, "_READONLY_partition_entry_array_crc32");
		JSON_LOOP_ITERATION_END(parse_gpt_sec_end, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(parse_gpt_sec_validate);

	// Parse disk UUID string
	if (disk_uuid_str) {
		nvmeibt_urn_uuid_str_to_union_uuid(&gpt->header.disk_obj_uuid, disk_uuid_str);
	}

	if (!entries_array) {
		N_Ef(parse_gpt_no_entries, "GPT section @STR has no entries array", section_name);
		return -1;
	}

	// Parse entries array
	n_entries_in_json = entries_array->array.len;
	fprintf(stdout, "  %s: %d entries in JSON\n", section_name, n_entries_in_json);

	for (j = 0; j < n_entries_in_json; j++) {
		struct mm_json_elem						*entry_elem = entries_array->array.elements[j];
		struct mm_json_dict						*entry_dict = NULL;
		int										entry_index = -1;
		struct nvmeibt_disk_gpt_partition_entry	temp_entry;

		if (entry_elem->type != JSON_E_DICT) {
			N_Wf(parse_entry_skip_not_dict, "Skipping entry @INT (not a dict)", j);
			continue;
		}

		// Get the index field to know where to place this entry
		entry_dict = &entry_elem->dict;
		for (i = 0; i < entry_dict->len; i++) {
			if (strcmp(entry_dict->elements[i].key, "index") == 0 &&
				entry_dict->elements[i].value->type == JSON_E_NUM) {
				entry_index = (int)entry_dict->elements[i].value->num;
				break;
			}
		}

		if (entry_index < 0 || entry_index >= gpt->max_n_entries) {
			N_Wf(parse_entry_bad_index, "Entry @INT has invalid index=@INT (max=@INT), skipping",
				 j, entry_index, gpt->max_n_entries);
			continue;
		}

		// Parse the entry
		if (parse_gpt_entry_from_json(&temp_entry, entry_elem) < 0) {
			N_Wf(parse_entry_failed, "Failed to parse entry @INT, skipping", j);
			continue;
		}

		// Copy to correct position in entries array
		memcpy(&gpt->entries[entry_index], &temp_entry, sizeof(temp_entry));
	}

	rv = 0;
	return rv;
}

/**
 * Phase 3: Execute EXPORT_JSON action
 */
static int execute_export_json(int disk_fd, struct gpt_util_config *config)
{
	int		rv;

	fprintf(stdout, "\nExporting GPT to JSON (copy=%s)...\n", gpt_copy_option_str(config->gpt_copy_option));

	rv = export_gpt_to_json(disk_fd, config, config->output_json_file);

	if (rv == 0) {
		fprintf(stdout, "\n" COL_GREEN "JSON export complete." COL_RESET " Edit the file and use --apply-from to restore.\n");
	}

	return rv;
}

/**
 * Validate that device has proper NVMesh structure (Main GPT + Metadata partition)
 * Returns 0 if valid, -1 if missing required structure
 */
static int validate_nvmesh_device_structure(int disk_fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e,
											 struct nvmeibt_disk_gpt *main_gpt,
											 const struct nvmeibt_disk_gpt_partition_entry **metadata_partition_out)
{
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;

	memset(main_gpt, 0, sizeof(*main_gpt));
	nvmeibt_strlcpy(main_gpt->main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt->main_or_metadata));

	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, pblk_size, main_gpt, pba_s, pba_hw_e, false) < 0) {
		N_Ef(validate_device_read_gpt_failed, "Failed to read Main GPT from device");
		return -1;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(main_gpt);
	if (!metadata_partition) {
		N_Ef(validate_device_no_metadata, "Device has no metadata partition (not a valid NVMesh device)");
		fprintf(stderr, COL_RED_BOLD "ERROR: Device must have EXCELERO_METADATA partition" COL_RESET "\n");
		return -1;
	}

	*metadata_partition_out = metadata_partition;
	return 0;
}

/**
 * Validate JSON has required sections for NVMesh apply
 * Returns 0 if valid, -1 if missing required sections
 */
static int validate_json_required_sections(struct mm_json_elem *json_root,
											const char *json_path,
											struct mm_json_elem **main_gpt_primary_out,
											struct mm_json_elem **metadata_gpt_primary_out)
{
	struct mm_json_elem *main_gpt_primary_elem;
	struct mm_json_elem *metadata_gpt_primary_elem;
	struct mm_json_elem *main_gpt_alternate_elem;

	main_gpt_primary_elem = json_get_dict_value(json_root, "main_gpt_primary");
	if (!main_gpt_primary_elem) {
		N_Ef(validate_json_no_main_gpt, "JSON missing required 'main_gpt_primary' section file=@STR", json_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON must contain main_gpt_primary section" COL_RESET "\n");
		return -1;
	}

	metadata_gpt_primary_elem = json_get_dict_value(json_root, "metadata_gpt_primary");
	if (!metadata_gpt_primary_elem) {
		N_Ef(validate_json_no_metadata_gpt, "JSON missing required 'metadata_gpt_primary' section file=@STR", json_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON must contain metadata_gpt_primary section for NVMesh devices" COL_RESET "\n");
		return -1;
	}

	/* Reject JSON with both primary and alternate (ambiguous which to use) */
	main_gpt_alternate_elem = json_get_dict_value(json_root, "main_gpt_alternate");
	if (main_gpt_alternate_elem) {
		N_Ef(validate_json_has_alternate, "JSON contains both primary and alternate copies (ambiguous) file=@STR", json_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON must contain only primary copy. Re-export with --gpt-copy=primary" COL_RESET "\n");
		return -1;
	}

	*main_gpt_primary_out = main_gpt_primary_elem;
	*metadata_gpt_primary_out = metadata_gpt_primary_elem;
	return 0;
}

/**
 * Prepare GPT structure from JSON section
 * Parses JSON, copies to GPT structure, calculates CRCs
 * Returns 0 on success, -1 on error
 */
static int prepare_gpt_from_json(struct nvmeibt_disk_gpt *gpt,
								 struct mm_json_elem *json_section,
								 const char *section_name,
								 int max_n_entries)
{
	memset(gpt, 0, sizeof(*gpt));
	nvmeibt_strlcpy(gpt->main_or_metadata, section_name, sizeof(gpt->main_or_metadata));
	gpt->max_n_entries = max_n_entries;

	if (parse_gpt_from_json_section(gpt, json_section, section_name) < 0) {
		N_Ef(prepare_gpt_parse_failed, "Failed to parse @STR from JSON", section_name);
		return -1;
	}

	/*
	 * Initialize static/constant fields (not in JSON, always the same)
	 * These are the same values set by nvmeibt_disk_metadata_init_gpt_structure()
	 */
	gpt->header.gpt_signature = GPT_SIGNATURE;
	gpt->header.revision = 0x00010000;			/* GPT revision by UEFI standard */
	gpt->header.header_size = 92;				/* GPT header size by UEFI standard */
	gpt->header.size_of_partition_entry = UEFI_MIN_GPT_ENTRY_SIZE;

	/* Calculate CRCs (exactly as store_gpt will do) */
	gpt->header.partition_entry_array_crc32 = crc32_seedless(gpt->entries,
															 gpt->header.n_partition_entries * gpt->header.size_of_partition_entry);
	gpt->header.header_crc32 = 0;
	gpt->header.header_crc32 = crc32_seedless(&gpt->header, gpt->header.header_size);

	return 0;
}

/**
 * Compare GPT structures and display diff
 * Returns number of changes detected
 */
static int compare_and_show_gpt_diff(const struct nvmeibt_disk_gpt *disk_gpt,
									 const struct nvmeibt_disk_gpt *json_gpt,
									 const char *gpt_name)
{
	int		i;
	int		n_additions = 0;
	int		n_deletions = 0;
	int		n_modifications = 0;
	int		n_unchanged = 0;

	fprintf(stdout, "\n=== %s Changes ===\n", gpt_name);
	fprintf(stdout, "CRC: header 0x%08x->0x%08x, entries 0x%08x->0x%08x\n",
			disk_gpt->header.header_crc32, json_gpt->header.header_crc32,
			disk_gpt->header.partition_entry_array_crc32, json_gpt->header.partition_entry_array_crc32);

	for (i = 0; i < json_gpt->max_n_entries; i++) {
		BOOL disk_in_use = nvmeibt_disk_metadata_is_gpt_entry_in_use(&disk_gpt->entries[i]);
		BOOL json_in_use = nvmeibt_disk_metadata_is_gpt_entry_in_use(&json_gpt->entries[i]);

		if (!disk_in_use && json_in_use) {
			show_entry_diff("ADD", i, NULL, &json_gpt->entries[i]);
			n_additions++;
		} else if (disk_in_use && !json_in_use) {
			show_entry_diff("DELETE", i, &disk_gpt->entries[i], NULL);
			n_deletions++;
		} else if (disk_in_use && json_in_use) {
			if (!entries_are_equal(&disk_gpt->entries[i], &json_gpt->entries[i])) {
				show_entry_diff("MODIFY", i, &disk_gpt->entries[i], &json_gpt->entries[i]);
				n_modifications++;
			} else {
				n_unchanged++;
			}
		}
	}

	fprintf(stdout, "\n" COL_WHITE_BOLD "%s Summary:" COL_RESET "\n", gpt_name);
	fprintf(stdout, "  Additions:     %d\n", n_additions);
	fprintf(stdout, "  Deletions:     %d\n", n_deletions);
	fprintf(stdout, "  Modifications: %d\n", n_modifications);
	fprintf(stdout, "  Unchanged:     %d\n", n_unchanged);

	if (n_additions + n_deletions + n_modifications == 0) {
		fprintf(stdout, COL_GREEN "No changes detected" COL_RESET "\n");
	}

	return (n_additions + n_deletions + n_modifications);
}

/**
 * Execute APPLY_JSON action
 * Parse JSON file, validate safety checks, and apply GPT changes
 * Default: dry-run (shows diff without writing)
 * With --write: applies changes to disk
 */
static int execute_apply_json(int disk_fd, struct gpt_util_config *config)
{
	int							rv = -1;
	int							json_fd = -1;
	struct nvmeibt_Str			*json_content = NULL;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt_primary_elem = NULL;
	struct mm_json_elem			*metadata_gpt_primary_elem = NULL;
	const char					*device_path_in_json = NULL;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition = NULL;
	struct nvmeibt_disk_gpt		current_main_gpt;
	struct nvmeibt_disk_gpt		current_metadata_gpt;
	struct nvmeibt_disk_gpt		json_main_gpt;
	struct nvmeibt_disk_gpt		json_metadata_gpt;
	int							n_main_changes = 0;
	int							n_metadata_changes = 0;

	fprintf(stdout, "\n=== Applying GPT from JSON: %s ===\n", config->apply_json_file);
	fprintf(stdout, "Device: %s\n", config->device_path);
	if (config->write_mode) {
		fprintf(stdout, "Mode: " COL_YELLOW "WRITE" COL_RESET " (changes will be applied to disk)\n");
	} else {
		fprintf(stdout, "Mode: DRY-RUN (use --write to apply)\n");
	}
	fprintf(stdout, "\n");

	/* Step 1: Read and parse JSON file */
	json_fd = NNVMEIBT_OPEN_READ(trace_apply_json_open, config->apply_json_file, 1);
	if (json_fd < 0) {
		N_Ef(apply_json_open_failed, "Failed to open JSON file @STR @AUTO_ERRNO", config->apply_json_file);
		goto out;
	}

	json_content = NNVMEIBT_STR_ALLOC(trace_apply_json_read);
	rv = NNVMEIBT_STR_FREAD_ATOMIC(trace_apply_json_fread, json_content, json_fd);
	if (rv < 0) {
		N_Ef(apply_json_read_failed, "Failed to read JSON file @STR @AUTO_ERRNO", config->apply_json_file);
		goto out;
	}
	NNVMEIBT_CLOSE(trace_apply_json_close, json_fd);
	json_fd = -1;

	json_root = parse_json_txt_into_kv_tree(nvmeibt_Str_str(json_content), nvmeibt_Str_strlen(json_content));
	if (!json_root || json_root->type != JSON_E_DICT) {
		N_Ef(apply_json_parse_failed, "Failed to parse JSON file @STR", config->apply_json_file);
		rv = -1;
		goto out;
	}

	/* Step 2: Validate device has required NVMesh structure */
	if (validate_nvmesh_device_structure(disk_fd, config->pblk_size, config->pba_s, config->pba_hw_e,
										  &current_main_gpt, &metadata_partition) < 0) {
		rv = -1;
		goto out;
	}

	/* Step 3: Validate JSON has required sections (fail fast) */
	if (validate_json_required_sections(json_root, config->apply_json_file,
										 &main_gpt_primary_elem, &metadata_gpt_primary_elem) < 0) {
		rv = -1;
		goto out;
	}

	/* Step 4: Validate device path matches */
	device_path_in_json = json_get_dict_str(json_root, "device_path", NULL);
	if (device_path_in_json && strcmp(device_path_in_json, config->device_path) != 0) {
		N_Ef(apply_json_device_mismatch, "Device path mismatch: JSON=@STR config=@STR",
			 device_path_in_json, config->device_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: Device path in JSON (%s) != device (%s)" COL_RESET "\n",
				device_path_in_json, config->device_path);
		rv = -1;
		goto out;
	}

	/* Step 5: Prepare Main GPT from JSON */
	if (prepare_gpt_from_json(&json_main_gpt, main_gpt_primary_elem, MAIN_GPT_NAME, LARGE_GPT_MAX_NUM_GPT_ENTRIES) < 0) {
		rv = -1;
		goto out;
	}

	/* Check GPT size compatibility */
	if (current_main_gpt.max_n_entries != json_main_gpt.max_n_entries) {
		N_Ef(apply_gpt_size_mismatch, "GPT size mismatch: JSON=@INT disk=@INT file=@STR dev=@STR",
			 json_main_gpt.max_n_entries, current_main_gpt.max_n_entries,
			 config->apply_json_file, config->device_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot apply GPT of different size (JSON=%d, disk=%d)" COL_RESET "\n",
				json_main_gpt.max_n_entries, current_main_gpt.max_n_entries);
		rv = -1;
		goto out;
	}

	/* Check for overlaps in JSON */
	if (detect_overlaps(json_main_gpt.entries, json_main_gpt.max_n_entries) > 0) {
		N_Ef(apply_json_overlap_block, "Overlaps detected in JSON, blocking apply. file=@STR", config->apply_json_file);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON contains overlapping partitions!" COL_RESET "\n");
		rv = -1;
		goto out;
	}

	/* Step 6: Prepare Metadata GPT from JSON */
	memset(&current_metadata_gpt, 0, sizeof(current_metadata_gpt));
	nvmeibt_strlcpy(current_metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(current_metadata_gpt.main_or_metadata));

	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &current_metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		N_Ef(apply_read_metadata_gpt_failed, "Failed to read current Metadata GPT dev=@STR", config->device_path);
		rv = -1;
		goto out;
	}

	if (prepare_gpt_from_json(&json_metadata_gpt, metadata_gpt_primary_elem, METADATA_GPT_NAME, MAX_NUM_GPT_ENTRIES) < 0) {
		rv = -1;
		goto out;
	}

	/* Step 7: Compare and show differences */
	n_main_changes = compare_and_show_gpt_diff(&current_main_gpt, &json_main_gpt, "Main GPT");
	fprintf(stdout, "\n");
	n_metadata_changes = compare_and_show_gpt_diff(&current_metadata_gpt, &json_metadata_gpt, "Metadata GPT");

	/* Step 8: Write if in write mode */
	if (config->write_mode) {
		fprintf(stdout, "\n" COL_YELLOW "=== Writing Changes to Disk ===" COL_RESET "\n");

		/*
		 * CRITICAL: Copy JSON data into current_main_gpt (which has correct location fields)
		 * JSON doesn't have location fields (my_pba, alternate_pba, partition_entry_pba)
		 * See gpt-util-implementation-notes.md Critical Caveat section
		 */
		memcpy(current_main_gpt.entries, json_main_gpt.entries, sizeof(current_main_gpt.entries));
		current_main_gpt.header.disk_obj_uuid = json_main_gpt.header.disk_obj_uuid;
		current_main_gpt.header.first_usable_pba = json_main_gpt.header.first_usable_pba;
		current_main_gpt.header.last_usable_pba = json_main_gpt.header.last_usable_pba;
		current_main_gpt.header.n_partition_entries = json_main_gpt.header.n_partition_entries;
		/* Location fields (my_pba, alternate_pba, partition_entry_pba) preserved from current_main_gpt */

		/* Write Main GPT - Audit trail log */
		N_IMf(apply_main_gpt_write, "Applying Main GPT from JSON: dev=@STR json=@STR changes=@INT CRC_new: hdr=@CRC ent=@CRC",
			  config->device_path, config->apply_json_file, n_main_changes,
			  json_main_gpt.header.header_crc32, json_main_gpt.header.partition_entry_array_crc32);

		if (nvmeibt_disk_metadata_store_gpt(NULL, disk_fd, config->pblk_size, &current_main_gpt, false) < 0) {
			N_Ef(apply_write_failed, "Failed to write Main GPT dev=@STR", config->device_path);
			fprintf(stderr, COL_RED_BOLD "ERROR: Failed to write Main GPT to disk" COL_RESET "\n");
			rv = -1;
			goto out;
		}

		/* Copy JSON data into current_metadata_gpt (same reason as above) */
		memcpy(current_metadata_gpt.entries, json_metadata_gpt.entries, sizeof(current_metadata_gpt.entries));
		current_metadata_gpt.header.disk_obj_uuid = json_metadata_gpt.header.disk_obj_uuid;
		current_metadata_gpt.header.first_usable_pba = json_metadata_gpt.header.first_usable_pba;
		current_metadata_gpt.header.last_usable_pba = json_metadata_gpt.header.last_usable_pba;
		current_metadata_gpt.header.n_partition_entries = json_metadata_gpt.header.n_partition_entries;

		/* Write Metadata GPT - Audit trail log */
		N_IMf(apply_metadata_gpt_write, "Applying Metadata GPT from JSON: dev=@STR json=@STR changes=@INT CRC_new: hdr=@CRC ent=@CRC",
			  config->device_path, config->apply_json_file, n_metadata_changes,
			  json_metadata_gpt.header.header_crc32, json_metadata_gpt.header.partition_entry_array_crc32);

		if (nvmeibt_disk_metadata_store_gpt(NULL, disk_fd, config->pblk_size, &current_metadata_gpt, false) < 0) {
			N_Ef(apply_write_metadata_gpt_failed, "Failed to write Metadata GPT dev=@STR", config->device_path);
			fprintf(stderr, COL_RED_BOLD "ERROR: Failed to write Metadata GPT to disk" COL_RESET "\n");
			rv = -1;
			goto out;
		}

		fprintf(stdout, "\n" COL_GREEN "=== GPT Successfully Updated ===" COL_RESET "\n");
		fprintf(stdout, "Device: %s\n", config->device_path);
		fprintf(stdout, "  Main GPT:     %d change%s\n", n_main_changes, n_main_changes == 1 ? "" : "s");
		fprintf(stdout, "  Metadata GPT: %d change%s\n", n_metadata_changes, n_metadata_changes == 1 ? "" : "s");
	} else {
		fprintf(stdout, "\n" COL_GREEN "=== Dry-Run Complete ===" COL_RESET "\n");
		if (n_main_changes + n_metadata_changes > 0) {
			fprintf(stdout, COL_YELLOW "Use --write flag to apply %d change%s to disk." COL_RESET "\n",
					n_main_changes + n_metadata_changes,
					(n_main_changes + n_metadata_changes) == 1 ? "" : "s");
		} else {
			fprintf(stdout, "No GPT changes detected - JSON matches disk.\n");
		}
	}

	rv = 0;

out:
	if (json_fd >= 0) {
		NNVMEIBT_CLOSE(trace_apply_json_cleanup_fd, json_fd);
	}
	// json_gpt.entries is a fixed array in the struct, not dynamically allocated
	if (json_root) {
		nvmeibt_mm_json_free_kv_tree(json_root);
	}
	NNVMEIBT_STR_FREE(trace_apply_json_cleanup_str, json_content);
	return rv;
}

/**
 * Phase 3: Execute DISPLAY_GPT action
 */
static int execute_display_gpt(int disk_fd, struct gpt_util_config *config)
{
	int		rv;

	// display_all_gpts() with fix_gpt=false (display only, no fix)
	rv = display_all_gpts(disk_fd, config->pblk_size, config->pba_s, config->pba_hw_e,
						  config->gpt_copy_option, config->device_path, false, config);

	return rv;
}

/**
 * Run GPT utility operation (normal mode - not self-test)
 * Uses 4-phase architecture: parse, validate, setup, execute
 */
static int run_gpt_util_op(int argc, char *argv[])
{
	int							rv = 1;
	int							disk_fd = -1;
	struct gpt_util_config		config;

	// Initialize config with defaults
	memset(&config, 0, sizeof(config));
	config.action = ACTION_DISPLAY_GPT;		// Default action
	config.gpt_copy_option = GPT_COPY_OPTION_PRIMARY;
	config.o_direct_mode = O_DIRECT_AUTO;	// Auto-detect O_DIRECT based on file type

	if (argc < 2) {
		print_usage(argv);
		goto out;
	}

	// ==================== PHASE 1: PARSE ====================
	if (parse_arguments(argc, argv, &config) < 0) {
		goto out;
	}

	// ==================== PHASE 2: VALIDATE ====================
	if (validate_config(&config) < 0) {
		print_usage(argv);
		goto out;
	}

	// ==================== PHASE 3: SETUP (OPEN DEVICE) ====================
	disk_fd = open_target_device(&config);
	if (disk_fd < 0) {
		goto out;
	}

	// ==================== PHASE 4: EXECUTE ====================
	fprintf(stdout, "\n");

	switch (config.action) {
	case ACTION_DISPLAY_GPT:
		rv = execute_display_gpt(disk_fd, &config);
		break;

	case ACTION_DISPLAY_MBR:
		rv = execute_display_mbr(disk_fd, &config);
		break;

	case ACTION_FIX_GPT:
		rv = execute_fix_gpt(disk_fd, &config);
		break;

	case ACTION_FIX_MBR:
		rv = execute_fix_mbr(disk_fd, &config);
		break;

	case ACTION_CHECK_EXCELERO:
		rv = execute_check_excelero(disk_fd, &config);
		break;

	case ACTION_UPGRADE_GPT:
		rv = execute_upgrade_gpt(disk_fd, &config);
		break;

	case ACTION_EXPORT_JSON:
		rv = execute_export_json(disk_fd, &config);
		break;

	case ACTION_APPLY_JSON:
		rv = execute_apply_json(disk_fd, &config);
		break;

	default:
		N_Ef(run_gpt_util_unknown_action, "Unknown action=@INT", config.action);
		rv = -1;
		break;
	}

out:
	if (disk_fd >= 0) {
		close(disk_fd);
	}

	return rv;
}

/**
 * Self-test wrapper for run_gpt_util_op (keeps production function static)
 * Exposed API for self-test framework to invoke gpt_util operations
 */
int SELF_TEST_run_gpt_util_op(int argc, char *argv[])
{
	return run_gpt_util_op(argc, argv);
}

/**
 * Self-test wrapper for upgrade_gpt_if_needed (keeps production function static)
 * Exposed API for self-test framework to test GPT upgrade logic
 */
int SELF_TEST_upgrade_gpt_if_needed(int disk_fd, int pblk_size, struct nvmeibt_disk_gpt *gpt, const char *gpt_name)
{
	return upgrade_gpt_if_needed(disk_fd, pblk_size, gpt, gpt_name);
}

/**
 * Main entry point for gpt_util
 * Dispatches to either self-test (with test selection support) or normal operation
 * Test selection: -T [tests] where tests can be "1", "1,3,5", "1-5", or empty for all
 */
int gpt_util_main(int argc, char *argv[])
{
	int			rv = 1;
	long		i;
	BOOL		is_self_test = false;
	const char	*test_selection = NULL;		// NULL = run all tests

	// Quick check for --self-test flag (before full parsing)
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--self-test") == 0) {
			is_self_test = true;
			// Check if next argument is a test selection (number, range, or list)
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				test_selection = argv[i + 1];
			}
			break;
		}
	}

	// Start trace pollers (gpt_util is a utility, routes traces properly)
	nvmeibt_start_all_trace_pollers(true);

#ifdef TOMA_SIMULATOR_SANDBOX
	// Initialize sandbox environment for subprogram execution
	{ extern void toma_unitest_env_start(void); toma_unitest_env_start(); }
#endif // #ifdef TOMA_SIMULATOR_SANDBOX

	// Buffer Manager
	if (nvmeibt_bm_create()) {
		N_Ef(gpt_util_bm_create_failed, "Failed to create buffer manager");
		return 1;
	}

	// Print version banner
	print_version_banner();

	// Dispatch to appropriate mode
	if (is_self_test) {
		rv = run_self_test(test_selection);
	} else {
		rv = run_gpt_util_op(argc, argv);
	}

	nvmeibt_bm_destroy();
	nvmeibt_join_all_trace_pollers();		// Clean shutdown of trace threads; beyond this point, no more traces

	return rv;
}

