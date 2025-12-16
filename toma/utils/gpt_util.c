/**
 * Utility that prints the GPT table of a given disk at a given offset and
 * prints it to the screen.
 *
 */

#include <ctype.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../nvmeibt_debug.h"
#include "../nvmeibt_disk_metadata.h"
#include "nvmeibt_bm.h"
#include "../nvmeibt_read_config.h"
#include "../nvmeibt_str.h"
#include "../nvmeibt_local_disk.h"
#include "nvmeibt_uuid.h"
#include "../nvmeibt_json_base.h"
#include "../interfaces/log/nvmeibt_binary_tracing.h"
#include "../interfaces/srvr/nvmeibt_srvr_proc.h"	// DISKS_INFO_FILE

#define GPT_UTIL_VERSION	"2.0.0-dev"
#define MAX_DEV_NAME		256

// Self-test mock device constants
#define SELF_TEST_MOCK_DEVICE_BLOCKS		2000		// 8MB at 4KB blocks (enough for nested GPT)
#define SELF_TEST_MOCK_DEVICE_BLOCK_SIZE	4096

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

// Forward declarations
static int upgrade_gpt_if_needed(int disk_fd, int pblk_size, struct nvmeibt_disk_gpt *gpt, const char *gpt_name);
static int detect_overlaps(const struct nvmeibt_disk_gpt_partition_entry *entries, int max_n_entries);
static int run_gpt_util_op(int argc, char *argv[]);
static int execute_apply_json(int disk_fd, struct gpt_util_config *config);

// Print test header banner (SELF-TEST only)
static void SELF_TEST_print_test_header(int test_idx, const char *description, const char *command)
{
	fprintf(stdout, "\n");
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_WHITE_BOLD "SELF-TEST %d: %s" COL_RESET "\n", test_idx, description);
	fprintf(stdout, "Emulated command: " COL_YELLOW "%s" COL_RESET "\n", command);
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
}

/**
 * Run a single test case (SELF-TEST only)
 * Creates device, marks persistent, runs gpt_util_op, reports PASSED/FAILED
 * Returns 0 on success, -1 on failure
 */
static int SELF_TEST_run_test_case(int *test_idx,
								   const char *description,
								   const char *test_device_path,
								   int (*setup_device)(const char *),
								   char **test_argv,
								   int test_argc)
{
	int		disk_fd;
	int		current_test = ++(*test_idx);		// Auto-increment counter

	// Use full command in test_argv[0] for display
	SELF_TEST_print_test_header(current_test, description, test_argv[0]);

	// Create/setup test device
	disk_fd = setup_device(test_device_path);
	if (disk_fd < 0) {
		N_Ef(run_test_setup_failed, "Failed to setup device for test @INT", current_test);
		fprintf(stdout, "\n" COL_RED_BOLD ">>> SELF-TEST %d: FAILED <<<" COL_RESET "\n", current_test);
		return -1;
	}

	close(disk_fd);

	// Run the test
	optind = 1;		// Reset getopt state
	if (run_gpt_util_op(test_argc, test_argv) != 0) {
		fprintf(stdout, "\n" COL_RED_BOLD ">>> SELF-TEST %d: FAILED <<<" COL_RESET "\n", current_test);
		return -1;
	}

	fprintf(stdout, "\n" COL_GREEN ">>> SELF-TEST %d: PASSED <<<" COL_RESET "\n", current_test);
	return 0;
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
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&header->disk_obj_uuid);
	nvmeibt_Str_sprintf(json_output, "    \"disk_uuid\": \"%s\",\n", urn_uuid.str);
	nvmeibt_Str_sprintf(json_output, "    \"n_partition_entries\": %d,\n", header->n_partition_entries);
	nvmeibt_Str_sprintf(json_output, "    \"first_usable_pba\": %lu,\n", header->first_usable_pba);
	nvmeibt_Str_sprintf(json_output, "    \"last_usable_pba\": %lu,\n", header->last_usable_pba);
	nvmeibt_Str_sprintf(json_output, "    \"entries\": [\n");

	// Export partition entries
	for (i = 0; i < max_n_entries; i++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i])) {
			const struct nvmeibt_disk_gpt_partition_entry *entry = &entries[i];
			struct nvmeibt_urn_uuid type_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_type_guid);
			struct nvmeibt_urn_uuid part_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_guid);

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
			nvmeibt_Str_sprintf(json_output, "        \"name\": \"%.72s\"\n", entry->partition_name);
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
	struct nvmeibt_disk_metadata			disk_md;
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
	BOOL									has_device_identifiers = false;

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
	nvmeibt_Str_sprintf(json_output, "  \"_human_edited\": false,\n");
	nvmeibt_Str_sprintf(json_output, "  \"_recalculate_crc\": false,\n");
	nvmeibt_Str_sprintf(json_output, "  \"_mismatch_detected\": %s,\n", is_mismatch ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "  \"_overlaps_detected\": %s,\n", has_overlaps ? "true" : "false");

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
	nvmeibt_Str_sprintf(json_output, "    \"signature\": \"0x%04x\",\n", mbr.signature);
	nvmeibt_Str_sprintf(json_output, "    \"os_type\": \"0x%02x\",\n", mbr.partitions[0].os_type);
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
			memset(&disk_md, 0, sizeof(disk_md));
			if (nvmeibt_disk_metadata_read_disk_metadata(NULL, disk_fd, config->pblk_size,
														 pbyte_s, &disk_md) == 0) {
				has_device_identifiers = true;
			}
		}

		// Export Metadata GPT
		if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
			BOOL is_last = !(config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) && !has_device_identifiers;
			export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_PRIMARY,
											metadata_bufs.primary_header,
											metadata_bufs.primary_entries,
											metadata_temp_gpt.max_n_entries, json_output, is_last);
		}
		if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
			BOOL is_last = !has_device_identifiers;
			export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_ALTERNATE,
											metadata_bufs.alternate_header,
											metadata_bufs.alternate_entries,
											metadata_temp_gpt.max_n_entries, json_output, is_last);
		}

		// Export device identifiers (if available)
		if (has_device_identifiers) {
			struct nvmeibt_urn_uuid nguid_urn;

			nvmeibt_Str_sprintf(json_output, "  \"device_identifiers\": {\n");
			nvmeibt_Str_sprintf(json_output, "    \"serial_id\": \"%s\",\n", disk_md.native_serial_str);

			nguid_urn = nvmeibt_union_uuid_to_urn_uuid(&disk_md.native_nguid_unused);
			nvmeibt_Str_sprintf(json_output, "    \"nguid\": \"%s\"\n", nguid_urn.str);
			nvmeibt_Str_sprintf(json_output, "  }");
		}

		free_gpt_buffers(&metadata_bufs);
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

	N_IMf(gpt_json_export_success, "GPT exported to JSON: dev=@STR file=@STR bytes=@SIZE_T copy_option=@STR has_metadata=@INT has_dev_id=@INT",
		  config->device_path, output_file, nvmeibt_Str_strlen(json_output), gpt_copy_option_str(config->gpt_copy_option), has_metadata_gpt, has_device_identifiers);
	fprintf(stdout, COL_GREEN "GPT exported to JSON: %s (%lu bytes)" COL_RESET "\n", output_file, nvmeibt_Str_strlen(json_output));

	// Build status message based on what was actually exported
	fprintf(stdout, "  - Exported: pMBR, Main GPT");
	if (has_metadata_gpt) {
		fprintf(stdout, ", Metadata GPT");
	}
	if (has_device_identifiers) {
		fprintf(stdout, ", Device Identifiers");
	}
	fprintf(stdout, "\n");

	// Warn if mismatch or overlaps detected
	if (is_mismatch) {
		N_Wf(gpt_json_mismatch_detected, "Mismatch detected in exported GPT: dev=@STR file=@STR",
			 config->device_path, output_file);
		fprintf(stdout, "\n");
		fprintf(stdout, COL_YELLOW "*** WARNING: Primary and alternate copies differ! ***" COL_RESET "\n");
		fprintf(stdout, "    JSON marked with '_mismatch_detected: true'\n");
		fprintf(stdout, "    Apply will be BLOCKED until you choose one copy.\n");
		fprintf(stdout, "    " COL_GREEN "Suggestion: Re-export with --gpt-copy=primary or --gpt-copy=alternate" COL_RESET "\n");
	}
	if (has_overlaps) {
		N_Wf(gpt_json_overlaps_detected, "Overlapping partitions detected in exported GPT: dev=@STR file=@STR",
			 config->device_path, output_file);
		fprintf(stdout, "\n");
		fprintf(stdout, COL_YELLOW "*** WARNING: Overlapping partitions detected! ***" COL_RESET "\n");
		fprintf(stdout, "    JSON marked with '_overlaps_detected: true'\n");
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
static int SELF_TEST_generate_and_open_mock_nvmesh_disk(const char *filepath)
{
	int									rv = -1;
	int									fd = -1;
	struct nvmeibt_disk_mbr				mbr;
	struct nvmeibt_disk_gpt				main_gpt;
	struct nvmeibt_disk_gpt				metadata_gpt;
	union nvmeib_uuid					disk_uuid;
	union nvmeib_uuid					metadata_disk_uuid;
	union nvmeib_uuid					metadata_partition_uuid;
	union nvmeib_uuid					disk_metadata_partition_uuid;
	uint64_t							n_disk_blocks = SELF_TEST_MOCK_DEVICE_BLOCKS;
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	struct nvmeibt_disk_gpt_partition_entry	*metadata_partition;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, "Metadata", sizeof(metadata_gpt.main_or_metadata));

	fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(selftest_create_failed, "Failed to create mock device @STR @AUTO_ERRNO", filepath);
		return -1;
	}

	// 1. Initialize protective MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		N_Ef(selftest_write_mbr_failed, "Failed to write MBR to mock device");
		goto out;
	}

	// 2. Initialize Main GPT structure using existing TOMA function
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	// 3. Add EXCELERO_METADATA partition using all available space (test device)
	{
		uint64_t metadata_start = main_gpt.header.first_usable_pba;
		uint64_t metadata_end = main_gpt.header.last_usable_pba;

		metadata_partition_uuid.ll[0] = 0xAABBCCDD11223344ULL;
		metadata_partition_uuid.ll[1] = 0x5566778899AABBCCULL;

		metadata_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
			&main_gpt, &EXCELERO_METADATA_PARTITION_TYPE_GUID,
			&metadata_partition_uuid,
			metadata_start, metadata_end,
			EXCELERO_METADATA_PARTITION_NAME,
			strlen(EXCELERO_METADATA_PARTITION_NAME));

		if (!metadata_partition) {
			N_Ef(selftest_add_metadata_failed, "Failed to add metadata partition to mock device");
			goto out;
		}
	}

	// 4. Write Main GPT to disk using existing TOMA function
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		N_Ef(selftest_store_main_gpt_failed, "Failed to store Main GPT to mock device");
		goto out;
	}

	// 5. Initialize nested Metadata GPT using existing TOMA functions
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
											 metadata_partition->pba_e,
											 &metadata_gpt, pblk_size,
											 MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	// Add EXCELERO_DISK_METADATA partition
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.last_usable_pba,
												 DISK_METADATA_PARTITION_NAME,
												 strlen(DISK_METADATA_PARTITION_NAME))) {
		N_Ef(selftest_add_disk_metadata_failed, "Failed to add disk_metadata partition to nested GPT");
		goto out;
	}

	// 6. Write Metadata GPT to disk
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &metadata_gpt, false) < 0) {
		N_Ef(selftest_store_metadata_gpt_failed, "Failed to store Metadata GPT to mock device");
		goto out;
	}

	// 7. Write disk metadata structure (for serial ID/NGUID testing)
	{
		const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
		struct nvmeibt_disk_metadata	disk_metadata;
		char							*dma_buffer = NULL;
		int								n_bytes_write;
		uint64_t						pbyte_s;

		disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
		if (disk_md_partition) {
			// Initialize disk metadata with test values
			memset(&disk_metadata, 0, sizeof(disk_metadata));
			disk_metadata.signature = DISK_METADATA_SIGNATURE;
			disk_metadata.format_pblk_size = pblk_size;
			disk_metadata.format_request_counter = 1;

			// Test serial ID and NGUID
			nvmeibt_strlcpy(disk_metadata.native_serial_str, "MOCK-SERIAL-12345678", sizeof(disk_metadata.native_serial_str));
			disk_metadata.native_nguid_unused.ll[0] = 0xAABBCCDD11223344ULL;
			disk_metadata.native_nguid_unused.ll[1] = 0x5566778899AABBCCULL;

			// Calculate CRC
			disk_metadata.crc32 = 0;
			disk_metadata.crc32 = crc32_seedless(&disk_metadata, sizeof(disk_metadata));

			// Write to disk
			pbyte_s = disk_md_partition->pba_s * pblk_size;
			n_bytes_write = roundup(sizeof(disk_metadata), pblk_size);
			dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_disk_md, PAGE_SIZE, n_bytes_write);
			memcpy(dma_buffer, &disk_metadata, sizeof(disk_metadata));

			if (pwrite(fd, dma_buffer, n_bytes_write, pbyte_s) != n_bytes_write) {
				N_Ef(selftest_write_disk_md_failed, "Failed to write disk metadata to mock device @AUTO_ERRNO");
				NNVMEIBT_BM_FREE(trace_selftest_disk_md_free, dma_buffer);
				goto out;
			}

			NNVMEIBT_BM_FREE(trace_selftest_disk_md_free2, dma_buffer);
			fprintf(stdout, "  - Disk metadata written (serial=%s)\n", disk_metadata.native_serial_str);
		}
	}

	fsync(fd);
	fprintf(stdout, "Mock NVMesh device created: %s (%lu blocks, %d KB)\n",
			filepath, n_disk_blocks, (int)(n_disk_blocks * pblk_size / 1024));
	fprintf(stdout, "  - Main GPT with EXCELERO_METADATA partition (LBA %lu-%lu)\n",
			metadata_partition->pba_s, metadata_partition->pba_e);
	fprintf(stdout, "  - Nested Metadata GPT with EXCELERO_DISK_METADATA partition\n");
	fprintf(stdout, "  - Disk metadata with test serial ID and NGUID\n\n");

	// Return the fd - caller will close it (which triggers sandbox auto-deletion)
	rv = fd;
	fd = -1;		// Don't close it in cleanup

out:
	if (fd >= 0) {
		close(fd);
	}
	return rv;
}

/**
 * Generate a mock device with intentionally overlapping partitions
 * Used to test overlap detection functionality
 * Returns the fd of the created device (caller must close it)
 */
static int SELF_TEST_generate_mock_device_with_overlaps(const char *filepath)
{
	int									rv = -1;
	int									fd = -1;
	struct nvmeibt_disk_mbr				mbr;
	struct nvmeibt_disk_gpt				main_gpt;
	union nvmeib_uuid					disk_uuid;
	union nvmeib_uuid					partition1_uuid;
	union nvmeib_uuid					partition2_uuid;
	uint64_t							n_disk_blocks = SELF_TEST_MOCK_DEVICE_BLOCKS;
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	uint64_t							overlap_start;
	uint64_t							overlap_end;

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));

	fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(selftest_overlap_create_failed, "Failed to create overlap test device @STR @AUTO_ERRNO", filepath);
		return -1;
	}

	// Initialize protective MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		N_Ef(selftest_overlap_write_mbr_failed, "Failed to write MBR to overlap test device");
		goto out;
	}

	// Initialize Main GPT
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	// Add first partition (LBA 258-1500)
	partition1_uuid.ll[0] = 0xAABBCCDD11223344ULL;
	partition1_uuid.ll[1] = 0x5566778899AABBCCULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&main_gpt,
												 &EXCELERO_METADATA_PARTITION_TYPE_GUID,
												 &partition1_uuid,
												 main_gpt.header.first_usable_pba,
												 main_gpt.header.first_usable_pba + 1242,  // 1500 - 258
												 "partition_1",
												 strlen("partition_1"))) {
		N_Ef(selftest_add_part1_failed, "Failed to add partition 1 to overlap test device");
		goto out;
	}

	// Add second partition (LBA 1400-1742) - OVERLAPS with first!
	partition2_uuid.ll[0] = 0x1122334455667788ULL;
	partition2_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	overlap_start = main_gpt.header.first_usable_pba + 1142;  // 1400
	overlap_end = main_gpt.header.last_usable_pba;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&main_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &partition2_uuid,
												 overlap_start,
												 overlap_end,
												 "partition_2_OVERLAP",
												 strlen("partition_2_OVERLAP"))) {
		N_Ef(selftest_add_part2_failed, "Failed to add partition 2 (overlap) to test device");
		goto out;
	}

	// Write Main GPT
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		N_Ef(selftest_store_overlap_gpt_failed, "Failed to store Main GPT with overlaps");
		goto out;
	}

	fsync(fd);
	fprintf(stdout, "Mock device created with OVERLAPPING partitions:\n");
	fprintf(stdout, "  Partition 1: LBA 258-1500\n");
	fprintf(stdout, "  Partition 2: LBA 1400-1742 (overlaps with partition 1)\n\n");

	rv = fd;
	fd = -1;

out:
	if (fd >= 0) {
		close(fd);
	}
	return rv;
}

/**
 * Corrupt the alternate GPT on an already-written mock device for mismatch testing
 * This creates a valid alternate with different content than primary (for testing mismatch detection)
 */
static int SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e)
{
	int						rv = -1;
	struct nvmeibt_disk_gpt	corrupted_gpt;

	memset(&corrupted_gpt, 0, sizeof(corrupted_gpt));
	nvmeibt_strlcpy(corrupted_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(corrupted_gpt.main_or_metadata));

	// 1. Read the existing GPT (uses existing TOMA API)
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, pblk_size, &corrupted_gpt,
										  pba_s, pba_hw_e, false) < 0) {
		N_Ef(selftest_read_for_corrupt_failed, "Failed to read GPT for mismatch corruption test");
		goto out;
	}

	// 2. Corrupt it: Change disk UUID (creates header mismatch)
	corrupted_gpt.header.disk_obj_uuid.ll[0] = 0xDEADBEEFDEADBEEFULL;
	corrupted_gpt.header.disk_obj_uuid.ll[1] = 0xCAFEBABECAFEBABEULL;

	// 3. Corrupt it: Change partition name in first entry (creates entries mismatch)
	str_to_char16_str("CORRUPTED_TEST",
					  strlen("CORRUPTED_TEST"),
					  corrupted_gpt.entries[0].partition_name);

	// 4. Write ONLY the alternate copy with corrupted data (uses TOMA API)
	if (nvmeibt_disk_metadata_store_gpt_one_copy(NULL, fd, pblk_size, &corrupted_gpt, true, false) < 0) {
		N_Ef(selftest_write_corrupt_alt_failed, "Failed to write corrupted alternate GPT for mismatch test");
		goto out;
	}

	fprintf(stdout, "  - Corrupted alternate GPT (different UUID and partition name)\n");
	fprintf(stdout, "  - Both copies have VALID CRCs but MISMATCHED content\n");

	rv = 0;

out:
	return rv;
}

/**
 * Write GPT header to disk at specified position (SELF-TEST ONLY)
 * Uses direct pwrite for self-test corruption scenarios
 */
static int SELF_TEST_write_gpt_header_at_position(int disk_fd, int pblk_size,
												  const struct nvmeibt_disk_gpt_header *header,
												  uint64_t header_pba)
{
	int			rv = -1;
	int			n_bytes;
	char		*dma_buffer = NULL;
	uint64_t	pbyte_s;
	ssize_t		written;

	n_bytes = roundup(sizeof(*header), pblk_size);
	pbyte_s = header_pba * pblk_size;

	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_write_hdr, PAGE_SIZE, n_bytes);
	memcpy(dma_buffer, header, sizeof(*header));

	written = pwrite(disk_fd, dma_buffer, n_bytes, pbyte_s);
	if (written != n_bytes) {
		N_Ef(selftest_pwrite_header_failed, "pwrite returned @RV_SSIZE_T (expected @INT) at PBA @ZX @AUTO_ERRNO",
			 written, n_bytes, header_pba);
		goto out;
	}

	rv = 0;

out:
	NNVMEIBT_BM_FREE(trace_selftest_write_hdr_free, dma_buffer);
	return rv;
}

/**
 * Corrupt Main GPT by setting n_partition_entries to a wrong value (e.g., 128)
 * but keeping CRC calculated with LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192)
 * This simulates the old buggy behavior where n_partition_entries was wrong
 * (but CRC was correctly calculated with the max value)
 * Returns 0 on success, -1 on error
 */
static int SELF_TEST_corrupt_gpt_n_partition_entries(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e,
													 int wrong_n_partition_entries)
{
	int							rv = -1;
	struct nvmeibt_disk_gpt		gpt;
	struct nvmeibt_disk_gpt_header	primary_header;
	struct nvmeibt_disk_gpt_header	alternate_header;
	uint32_t					crc_with_max_entries;
	int							nbytes;
	int							original_n_partition_entries;

	memset(&gpt, 0, sizeof(gpt));
	nvmeibt_strlcpy(gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt.main_or_metadata));

	// 1. Read the existing GPT
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, pblk_size, &gpt,
										  pba_s, pba_hw_e, false) < 0) {
		N_Ef(selftest_read_for_n_part_corrupt_failed, "Failed to read GPT for n_partition_entries corruption test");
		goto out;
	}

	original_n_partition_entries = gpt.header.n_partition_entries;

	// 2. Calculate CRC using LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192) - the correct/max value
	nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * gpt.header.size_of_partition_entry;
	crc_with_max_entries = crc32_seedless(gpt.entries, nbytes);

	fprintf(stdout, "  - Corrupting GPT n_partition_entries:\n");
	fprintf(stdout, "    Setting n_partition_entries: %d -> %d (wrong)\n",
			original_n_partition_entries, wrong_n_partition_entries);
	fprintf(stdout, "    CRC calculated with %d entries (correct max): 0x%08x\n",
			LARGE_GPT_MAX_NUM_GPT_ENTRIES, crc_with_max_entries);

	// 3. Prepare primary header with wrong n_partition_entries but CRC from max entries
	memcpy(&primary_header, &gpt.header, sizeof(primary_header));
	primary_header.n_partition_entries = wrong_n_partition_entries;
	primary_header.partition_entry_array_crc32 = crc_with_max_entries;
	primary_header.header_crc32 = 0;
	primary_header.header_crc32 = crc32_seedless(&primary_header, sizeof(primary_header));

	// 4. Prepare alternate header with wrong n_partition_entries but CRC from max entries
	memcpy(&alternate_header, &gpt.header, sizeof(alternate_header));
	alternate_header.n_partition_entries = wrong_n_partition_entries;
	alternate_header.partition_entry_array_crc32 = crc_with_max_entries;
	alternate_header.my_pba = gpt.header.alternate_pba;
	alternate_header.alternate_pba = gpt.header.my_pba;
	{
		int64_t	primary_entries_offset = (int64_t)gpt.header.partition_entry_pba - (int64_t)gpt.header.my_pba;
		alternate_header.partition_entry_pba = gpt.header.alternate_pba + primary_entries_offset;
		if (alternate_header.partition_entry_pba >= alternate_header.my_pba) {
			int entries_n_pblks = divroundup(nbytes, pblk_size);
			alternate_header.partition_entry_pba = alternate_header.my_pba - entries_n_pblks;
		}
	}
	alternate_header.header_crc32 = 0;
	alternate_header.header_crc32 = crc32_seedless(&alternate_header, sizeof(alternate_header));

	// 5. Write corrupted primary header
	if (SELF_TEST_write_gpt_header_at_position(fd, pblk_size, &primary_header, gpt.header.my_pba) < 0) {
		N_Ef(selftest_write_corrupt_pri_hdr_failed, "Failed to write corrupted primary header for n_part test");
		goto out;
	}

	// 6. Write corrupted alternate header
	if (SELF_TEST_write_gpt_header_at_position(fd, pblk_size, &alternate_header, gpt.header.alternate_pba) < 0) {
		N_Ef(selftest_write_corrupt_alt_hdr_failed, "Failed to write corrupted alternate header for n_part test");
		goto out;
	}
	fsync(fd);
	fprintf(stdout, "  - GPT now has n_partition_entries=%d but CRC calculated with %d entries\n",
			wrong_n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	rv = 0;

out:
	return rv;
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

// Setup device with mismatch (for test 2)
static int SELF_TEST_setup_device_with_mismatch(const char *filepath)
{
	int		fd;

	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(filepath);
	if (fd < 0) {
		return -1;
	}
	if (SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(fd,
														  SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
														  1,
														  SELF_TEST_MOCK_DEVICE_BLOCKS - 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/**
 * Run comprehensive self-test suite
 * Creates mock devices and calls run_gpt_util_op() with simulated parameters
 * This tests the complete production code path end-to-end
 */
static int run_self_test(void)
{
	int			rv = 1;
	int			disk_fd = -1;
	int			test_idx = 0;		// Auto-incrementing test counter
	const char	*test_device_path;
	char		*test_argv[10];
	int			test_argc;

	mkdir(TOMA_ROOT_DIR "tmp", 0755);
	test_device_path = TOMA_ROOT_DIR "tmp/gpt_util_self_test";

	// ===== TEST 1: Normal GPT =====
	if (1) {
		test_argv[0] = "gpt_util -a <path> -c both";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-c";
		test_argv[4] = "both";
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "Normal GPT (Primary == Alternate)",
									 test_device_path,
									 SELF_TEST_generate_and_open_mock_nvmesh_disk,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== TEST 2: Mismatched GPT (mismatch expected) =====
	if (1) {
		test_argv[0] = "gpt_util -a <path> -c both";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-c";
		test_argv[4] = "both";
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "Mismatched GPT (Primary != Alternate)",
									 test_device_path,
									 SELF_TEST_setup_device_with_mismatch,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== TEST 3: UUID Filtering =====
	if (1) {
		test_argv[0] = "gpt_util -a <path> --filter-uuid <UUID>";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "--filter-uuid";
		test_argv[4] = "aabbccdd-1122-3344-5566-778899aabbcc";		// EXCELERO_METADATA partition UUID
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "UUID Filtering",
									 test_device_path,
									 SELF_TEST_generate_and_open_mock_nvmesh_disk,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== TEST 4: LBA Filtering =====
	if (1) {
		test_argv[0] = "gpt_util -a <path> --filter-lba 1000";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "--filter-lba";
		test_argv[4] = "1000";
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "LBA Filtering",
									 test_device_path,
									 SELF_TEST_generate_and_open_mock_nvmesh_disk,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== TEST 5: Overlap Detection =====
	if (1) {
		test_argv[0] = "gpt_util -a <path> -c both";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-c";
		test_argv[4] = "both";
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "Overlap Detection",
									 test_device_path,
									 SELF_TEST_generate_mock_device_with_overlaps,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== TEST 6: GPT Upgrade (corrupt → upgrade → verify) =====
	if (1) {
		struct nvmeibt_disk_gpt		main_gpt;
		struct nvmeibt_disk_gpt		verify_gpt;
		uint32_t					expected_crc;
		int							nbytes;
		int							wrong_n_partition_entries = 128;
		char						description[128];

		test_idx++;		// Increment for test 6
		snprintf(description, sizeof(description),
				 "GPT Upgrade (corrupt n_partition_entries to %d, upgrade to %d)",
				 wrong_n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);
		SELF_TEST_print_test_header(test_idx, description, "Internal API test (corrupt → upgrade → verify)");

		// Create fresh device
		disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(test_device_path);
		if (disk_fd < 0) {
			N_Ef(selftest6_gen_device_failed, "Failed to generate test device for GPT upgrade test");
			goto out;
		}

		// Corrupt Main GPT by setting n_partition_entries to 128
		if (SELF_TEST_corrupt_gpt_n_partition_entries(disk_fd,
													  SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
													  1,
													  SELF_TEST_MOCK_DEVICE_BLOCKS - 1,
													  wrong_n_partition_entries) < 0) {
			N_Ef(selftest6_corrupt_n_part_failed, "Failed to corrupt Main GPT n_partition_entries for test 6");
			close(disk_fd);
			goto out;
		}

		// Read the corrupted GPT
		fprintf(stdout, "\nReading corrupted GPT...\n");
		memset(&main_gpt, 0, sizeof(main_gpt));
		nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));

		if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
											  &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			N_Ef(selftest6_read_corrupt_failed, "Failed to read corrupted GPT for test 6");
			close(disk_fd);
			goto out;
		}

		// Verify corruption: n_partition_entries should be wrong, but CRC should match max entries
		nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * main_gpt.header.size_of_partition_entry;
		expected_crc = crc32_seedless(main_gpt.entries, nbytes);

		fprintf(stdout, "  Before upgrade:\n");
		fprintf(stdout, "    n_partition_entries=%d (wrong, should be %d)\n",
				main_gpt.header.n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);
		fprintf(stdout, "    CRC=0x%08x (calculated with %d entries: 0x%08x)\n",
				main_gpt.header.partition_entry_array_crc32, LARGE_GPT_MAX_NUM_GPT_ENTRIES, expected_crc);

		if (main_gpt.header.n_partition_entries != wrong_n_partition_entries) {
			fprintf(stderr, "GPT doesn't have wrong n_partition_entries - corruption failed\n");
			fprintf(stderr, "  Expected %d, got %d\n", wrong_n_partition_entries, main_gpt.header.n_partition_entries);
			close(disk_fd);
			goto out;
		}

		if (main_gpt.header.partition_entry_array_crc32 != expected_crc) {
			fprintf(stderr, "CRC doesn't match max entries calculation - corruption failed\n");
			close(disk_fd);
			goto out;
		}

		// Perform the upgrade
		fprintf(stdout, "\nPerforming GPT upgrade...\n");
		if (upgrade_gpt_if_needed(disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, "Main") < 0) {
			N_Ef(selftest6_upgrade_failed, "GPT upgrade failed in test 6");
			close(disk_fd);
			goto out;
		}

		// Re-read GPT to verify upgrade
		fprintf(stdout, "\nVerifying GPT upgrade...\n");
		memset(&verify_gpt, 0, sizeof(verify_gpt));
		nvmeibt_strlcpy(verify_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(verify_gpt.main_or_metadata));

		if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
											  &verify_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			N_Ef(selftest6_verify_read_failed, "Failed to read GPT for verification in test 6");
			close(disk_fd);
			goto out;
		}

		close(disk_fd);
		disk_fd = -1;

		fprintf(stdout, "  After upgrade: n_partition_entries=%d (expected %d)\n",
				verify_gpt.header.n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);

		// Check 1: n_partition_entries should be fixed to LARGE_GPT_MAX_NUM_GPT_ENTRIES
		if (verify_gpt.header.n_partition_entries != LARGE_GPT_MAX_NUM_GPT_ENTRIES) {
			fprintf(stderr, "FAIL: n_partition_entries not fixed!\n");
			fprintf(stderr, "      Expected %d, got %d\n",
					LARGE_GPT_MAX_NUM_GPT_ENTRIES, verify_gpt.header.n_partition_entries);
			fprintf(stdout, "\n>>> SELF-TEST %d: FAILED (n_partition_entries not fixed) <<<\n", test_idx);
			goto out;
		}
		fprintf(stdout, "  PASS: n_partition_entries fixed to %d\n", LARGE_GPT_MAX_NUM_GPT_ENTRIES);

		// Check 2: CRC should be correct for the new n_partition_entries
		nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * verify_gpt.header.size_of_partition_entry;
		expected_crc = crc32_seedless(verify_gpt.entries, nbytes);

		fprintf(stdout, "  CRC check: Stored=0x%08x, Expected=0x%08x\n",
				verify_gpt.header.partition_entry_array_crc32, expected_crc);

		if (verify_gpt.header.partition_entry_array_crc32 != expected_crc) {
			fprintf(stderr, "FAIL: CRC mismatch after upgrade!\n");
			fprintf(stdout, "\n>>> SELF-TEST %d: FAILED (CRC mismatch) <<<\n", test_idx);
			goto out;
		}
		fprintf(stdout, "  PASS: CRC is correct for n_partition_entries=%d\n", LARGE_GPT_MAX_NUM_GPT_ENTRIES);

		fprintf(stdout, "\n  GPT upgrade verified: n_partition_entries fixed from %d to %d!\n",
				wrong_n_partition_entries, LARGE_GPT_MAX_NUM_GPT_ENTRIES);
		fprintf(stdout, "\n>>> SELF-TEST %d: PASSED <<<\n", test_idx);
	}

	if (1) {
		// ===== TEST 7: JSON Export =====
		test_argv[0] = "gpt_util -a <path> -J <file>";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-J";
		test_argv[4] = "./test_export.json";
		test_argc = 5;

		if (SELF_TEST_run_test_case(&test_idx, "JSON Export",
									 test_device_path,
									 SELF_TEST_generate_and_open_mock_nvmesh_disk,
									 test_argv, test_argc) < 0) {
			goto out;
		}

		// ===== TEST 8: JSON Apply (Dry-Run) =====
		test_argv[0] = "gpt_util -a <path> --apply-from <file>";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "--apply-from";
		test_argv[4] = "./test_export.json";
		test_argc = 5;

		// Relies on Test 7's JSON file, but creates fresh device (tests idempotence)
		if (SELF_TEST_run_test_case(&test_idx, "JSON Apply - Dry-Run",
									 test_device_path,
									 SELF_TEST_generate_and_open_mock_nvmesh_disk,
									 test_argv, test_argc) < 0) {
			goto out;
		}
	}

	// ===== SUMMARY =====
	fprintf(stdout, "\n");
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_GREEN "ALL SELF-TESTS PASSED (%d/%d)" COL_RESET "\n", test_idx, test_idx);
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");

	rv = 0;

out:
	if (disk_fd >= 0) {
		close(disk_fd);
	}

	// Always clean up test file (even on failure)
	unlink(test_device_path);

	return rv;
}

static void print_usage(char *argv[])
{
	fprintf(stdout, "Usage: %s [OPTIONS]\n\n", argv[0]);

	fprintf(stdout, "Device Context:\n");
	fprintf(stdout, "  -d, --device=PATH           NVMesh managed device (reads disks.csv)\n");
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
	fprintf(stdout, "  --filter-lba=ADDR           Show only entries containing LBA address\n\n");

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
		{"output-json",				required_argument,	0,	'J'},
		{"apply-from",				required_argument,	0,	'A'},
		{"write",					no_argument,		0,	'W'},
		{"direct",					no_argument,		0,	'D'},
		{"no-direct",				no_argument,		0,	'N'},

		{0, 0, 0, 0}
	};
	static const char short_options[] = "d:a:s:e:b:c:u:l:J:A:imfFUW";
	static int long_idx = -1;

	for (i = 0; i < argc; ++i) {
		_argv[i] = trim_whitespace(argv[i]);
	}

	new_config = NNVMEIBT_STR_ALLOC(trace_parse_args_config);
	while ((op = getopt_long(argc, _argv, short_options, long_options, &long_idx)) != -1) {
		const char				file_name[] = DISKS_INFO_FILE;
		int						fd = -1;
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
			// Read disks.csv
			if ((fd = NNVMEIBT_OPEN_READ(trace_12_main, file_name, 1)) < 0) {
				N_Ef(parse_args_open_csv_failed, "Failed to open @FILE_ENTRY_NAME @AUTO_ERRNO", file_name);
				rv = -1;
				goto out;
			}
			// Read disks file data
			nvmeibt_Str_reuse(new_config);
			rv = NNVMEIBT_STR_FREAD_ATOMIC(trace_13_main, new_config, fd);
			if (rv < 0) {
				N_Ef(t_zzz_26, "Error reading the file @FILE_ENTRY_NAME, @AUTO_ERRNO", file_name);
				rv = -1;
				goto out;
			}

			// Parse disks.csv to find device parameters
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
					char *ref_header = (char *)nvmeibt_get_csv_header_by_section_type(section_type);
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
						fprintf(stdout,"Found device=%s in disks.csv, using pba_s=0x%lx, pba_e=0x%lx, block_size=%d\n",
								config->device_path, config->pba_s, config->pba_e, config->pblk_size);
						break;
					} else {
						fprintf(stdout, "Device=%s is different from requested=%s skipping\n",
								disk_config.dev_file_name, config->device_path);
					}
				}
			}

			if (fd >= 0) {
				NNVMEIBT_CLOSE(trace_15_main, fd);
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
 * Phase 3: Execute APPLY_JSON action
 * Parse JSON file, validate safety checks, and apply GPT changes
 * Default: dry-run (show changes without writing)
 * With --write: actually apply changes after confirmation
 */
static int execute_apply_json(int disk_fd, struct gpt_util_config *config)
{
	int							rv = -1;
	struct nvmeibt_Str			*json_content = NULL;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_kv_pair		*kv = NULL;
	struct mm_json_dict			*dict = NULL;
	int							json_fd = -1;
	BOOL						mismatch_detected = false;
	BOOL						overlaps_detected = false;
	BOOL						human_edited = false;
	BOOL						recalculate_crc = false;
	const char					*device_path_in_json = NULL;
	struct mm_json_elem			*main_gpt_primary_elem = NULL;
	struct mm_json_elem			*main_gpt_alternate_elem = NULL;
	int							n_gpt_sections_found = 0;
	struct nvmeibt_disk_gpt		current_gpt;
	struct nvmeibt_disk_gpt		json_gpt;
	JSON_ASSIGN_AND_CALL_INIT();		// Declares: JSON_ARR, json_n, is_found, __json_iter, n_json_tokens

	(void)disk_fd;		// Will be used in next step

	fprintf(stdout, "\n=== Applying GPT from JSON: %s ===\n", config->apply_json_file);

	if (config->write_mode) {
		fprintf(stdout, "Mode: WRITE (changes will be applied to disk)\n");
	} else {
		fprintf(stdout, "Mode: DRY-RUN (showing changes, use --write to apply)\n");
	}
	fprintf(stdout, "\n");

	// Step 1: Read JSON file
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

	fprintf(stdout, "JSON file read successfully (%zu bytes)\n\n", nvmeibt_Str_strlen(json_content));

	// Step 2: Parse JSON into key-value tree
	json_root = parse_json_txt_into_kv_tree(nvmeibt_Str_str(json_content), nvmeibt_Str_strlen(json_content));
	if (!json_root) {
		N_Ef(apply_json_parse_failed, "Failed to parse JSON file @STR", config->apply_json_file);
		rv = -1;
		goto out;
	}

	if (json_root->type != JSON_E_DICT) {
		N_Ef(apply_json_not_dict, "JSON root is not a dictionary type=@INT", json_root->type);
		rv = -1;
		goto out;
	}

	fprintf(stdout, "JSON parsed successfully\n\n");

	// Step 3: Extract and validate metadata using JSON macros
	fprintf(stdout, "=== Validating JSON metadata ===\n");

	dict = &json_root->dict;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(apply_json_meta, kv->key);
		JSON_ASSIGN_PLAIN(apply_dev_path, "device_path", device_path_in_json, kv->value->str);
		JSON_ASSIGN_PLAIN(apply_mismatch, "_mismatch_detected", mismatch_detected, (kv->value->num != 0));
		JSON_ASSIGN_PLAIN(apply_overlaps, "_overlaps_detected", overlaps_detected, (kv->value->num != 0));
		JSON_ASSIGN_PLAIN(apply_human_ed, "_human_edited", human_edited, (kv->value->num != 0));
		JSON_ASSIGN_PLAIN(apply_recalc, "_recalculate_crc", recalculate_crc, (kv->value->num != 0));
		JSON_ASSIGN_OPTIONAL(apply_timestamp, "backup_timestamp");
		JSON_ASSIGN_PLAIN(apply_main_pri, "main_gpt_primary", main_gpt_primary_elem, kv->value);
		JSON_ASSIGN_PLAIN(apply_main_alt, "main_gpt_alternate", main_gpt_alternate_elem, kv->value);
		JSON_LOOP_ITERATION_END(apply_json_meta_end, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(apply_json_meta_validate);

	// Device path validation
	if (device_path_in_json) {
		fprintf(stdout, "Device in JSON: %s\n", device_path_in_json);
		fprintf(stdout, "Device in config: %s\n", config->device_path);
		if (strcmp(device_path_in_json, config->device_path) != 0) {
			N_Ef(apply_json_device_mismatch, "Device path mismatch: JSON=@STR config=@STR",
				 device_path_in_json, config->device_path);
			rv = -1;
			goto out;
		}
	}

	// Display detected flags
	if (mismatch_detected) {
		fprintf(stdout, "_mismatch_detected: true\n");
	}
	if (overlaps_detected) {
		fprintf(stdout, "_overlaps_detected: true\n");
	}
	if (human_edited) {
		fprintf(stdout, "_human_edited: true\n");
	}
	if (recalculate_crc) {
		fprintf(stdout, "_recalculate_crc: true\n");
	}

	// Safety check: Block if mismatch detected and not resolved
	if (mismatch_detected) {
		N_Ef(apply_json_mismatch_block, "JSON has _mismatch_detected=true, blocking apply. file=@STR",
			 config->apply_json_file);
		rv = -1;
		goto out;
	}

	// Safety check: Block if overlaps detected
	if (overlaps_detected) {
		N_Ef(apply_json_overlap_block, "JSON has _overlaps_detected=true, blocking apply. file=@STR",
			 config->apply_json_file);
		rv = -1;
		goto out;
	}

	fprintf(stdout, "\n" COL_GREEN "=== Metadata validation: PASSED ===" COL_RESET "\n\n");

	// Step 4: Parse GPT sections and prepare for apply
	memset(&current_gpt, 0, sizeof(current_gpt));
	memset(&json_gpt, 0, sizeof(json_gpt));
	nvmeibt_strlcpy(current_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(current_gpt.main_or_metadata));
	nvmeibt_strlcpy(json_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(json_gpt.main_or_metadata));
	json_gpt.max_n_entries = LARGE_GPT_MAX_NUM_GPT_ENTRIES;

	fprintf(stdout, "=== Parsing GPT sections ===\n");

	// Determine which GPT section(s) to apply (already extracted in Step 3)
	if (main_gpt_primary_elem) {
		n_gpt_sections_found++;
		fprintf(stdout, "Found: main_gpt_primary\n");
	}
	if (main_gpt_alternate_elem) {
		n_gpt_sections_found++;
		fprintf(stdout, "Found: main_gpt_alternate\n");
	}

	if (n_gpt_sections_found == 0) {
		N_Ef(apply_json_no_gpt_sections, "No GPT sections found in JSON file=@STR", config->apply_json_file);
		rv = -1;
		goto out;
	}

	if (n_gpt_sections_found == 2) {
		// Both primary and alternate present - this shouldn't happen if mismatch check passed
		N_Wf(apply_json_both_sections, "JSON has both primary and alternate sections (will apply both) file=@STR",
			 config->apply_json_file);
	}

	fprintf(stdout, "\n");

	// Step 5: Read current GPT from disk
	fprintf(stdout, "=== Reading current GPT from disk ===\n");
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &current_gpt,
										  config->pba_s, config->pba_hw_e, false) < 0) {
		N_Ef(apply_json_read_current_failed, "Failed to read current GPT from disk dev=@STR", config->device_path);
		goto out;
	}
	fprintf(stdout, "Current GPT: n_entries=%d, first_usable=%lu, last_usable=%lu\n",
			current_gpt.header.n_partition_entries,
			current_gpt.header.first_usable_pba,
			current_gpt.header.last_usable_pba);

	// Step 6: Parse GPT from JSON
	fprintf(stdout, "\n=== Parsing GPT from JSON ===\n");

	// Parse the GPT section(s) - entries are already part of struct, no separate allocation needed
	if (main_gpt_primary_elem) {
		if (parse_gpt_from_json_section(&json_gpt, main_gpt_primary_elem, "main_gpt_primary") < 0) {
			N_Ef(apply_json_parse_primary_failed, "Failed to parse main_gpt_primary from JSON");
			goto out;
		}
	} else if (main_gpt_alternate_elem) {
		if (parse_gpt_from_json_section(&json_gpt, main_gpt_alternate_elem, "main_gpt_alternate") < 0) {
			N_Ef(apply_json_parse_alternate_failed, "Failed to parse main_gpt_alternate from JSON");
			goto out;
		}
	}

	// Step 7: Compare and show diff
	fprintf(stdout, "\n=== Comparing GPT changes ===\n");
	fprintf(stdout, "TODO: Implement detailed diff\n");
	fprintf(stdout, "TODO: Count additions, deletions, modifications\n");
	fprintf(stdout, "\n");

	// Step 8: Write changes if in write mode
	if (config->write_mode) {
		fprintf(stdout, "=== Would apply changes (NOT IMPLEMENTED YET) ===\n");
		fprintf(stdout, "TODO: Prompt for confirmation\n");
		fprintf(stdout, "TODO: Copy json_gpt to current_gpt\n");
		fprintf(stdout, "TODO: Call nvmeibt_disk_metadata_store_gpt()\n");
		fprintf(stdout, "TODO: Log audit trail with N_IMf (before/after CRCs)\n");
	} else {
		fprintf(stdout, COL_GREEN "=== Dry-run complete ===" COL_RESET "\n");
		fprintf(stdout, "No changes written to disk.\n");
		fprintf(stdout, COL_YELLOW "Use --write flag to actually apply changes." COL_RESET "\n");
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
 * Main entry point for gpt_util
 * Dispatches to either self-test or normal operation
 */
int gpt_util_main(int argc, char *argv[])
{
	int		rv = 1;
	long	i;
	BOOL	is_self_test = false;

	// Quick check for --self-test flag (before full parsing)
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--self-test") == 0) {
			is_self_test = true;
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
		rv = run_self_test();
	} else {
		rv = run_gpt_util_op(argc, argv);
	}

	nvmeibt_bm_destroy();
	nvmeibt_join_all_trace_pollers();		// Clean shutdown of trace threads; beyond this point, no more traces

	return rv;
}

