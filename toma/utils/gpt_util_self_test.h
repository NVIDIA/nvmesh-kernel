/**
 * gpt_util_self_test.h - Self-test framework for gpt_util
 *
 * Provides comprehensive test suite for GPT utility operations
 */

#ifndef NVMEIBT_GPT_UTIL_SELF_TEST_H
#define NVMEIBT_GPT_UTIL_SELF_TEST_H

#include "../nvmeibt_common.h"
#include "../nvmeibt_disk_metadata.h"
#include "../nvmeibt_local_disk.h"

// Self-test wrappers for production functions (from gpt_util.c)
// These wrappers allow tests to call production functions while keeping them static/encapsulated
int SELF_TEST_run_gpt_util_op(int argc, char *argv[]);
int SELF_TEST_upgrade_gpt_if_needed(int disk_fd, int pblk_size, struct nvmeibt_disk_gpt *gpt, const char *gpt_name);

// Helper function for generating consistent mock serials in tests
void SELF_TEST_generate_mock_serial_number_from_path(const char *device_path, char *serial_out, size_t size);

// Self-test mock device constants
#define SELF_TEST_MOCK_DEVICE_BLOCKS		2000		// 8MB at 4KB blocks (enough for nested GPT)
#define SELF_TEST_MOCK_DEVICE_BLOCK_SIZE	4096
#define GPT_UTIL_BACKUP_DIR TOMA_ROOT_DIR "var/opt/nvmesh/toma"

// Self-test framework: Test context structure
struct self_test_ctx {
	const char	*test_device_path;
	const char	*wrong_device_path;
	char		**test_argv;
	int			*test_argc;
	BOOL		quiet_mode;
};

// Self-test framework: Test function signature
typedef int (*self_test_func_t)(struct self_test_ctx *ctx);

// Self-test framework: Test registration structure
struct self_test_entry {
	const char			*name;
	const char			*command;
	self_test_func_t	func;
};

// X-Macro: Declare all tests here (order determines test numbers automatically)
// Format: X(function_name, "Test Name", "Command Description")
// Note: Negative tests (expect blocking) handle result inversion internally
#define SELF_TEST_LIST \
	/* Display & Filtering */ \
	X(normal_gpt, "Normal GPT (Primary == Alternate)", "gpt_util -a <path> -c both") \
	X(mismatch_gpt, "Mismatched GPT (Display + Export + Flag Validation)", "gpt_util -a <path> -c both + export") \
	X(uuid_filtering, "UUID Filtering", "gpt_util -a <path> --filter-uuid <UUID>") \
	X(lba_filtering, "LBA Filtering", "gpt_util -a <path> --filter-lba 1000") \
	X(overlap_detection, "Overlap Detection (Display + Export + Flag Validation)", "gpt_util -a <path> -c both + export") \
	/* Core Operations */ \
	X(gpt_upgrade, "GPT Upgrade (corrupt n_partition_entries to 128, upgrade to 8192)", "Internal API test") \
	X(json_export_apply, "JSON Export + Apply (Dry-Run)", "gpt_util -a <path> -J + --apply-from") \
	X(zeroing_verify, "Zeroing Verification Commands (-Z)", "gpt_util -a <path> -Z") \
	X(diff_no_changes, "Diff Comparison - No Changes", "gpt_util -a <path> -J + --apply-from") \
	X(diff_modifications, "Diff Comparison - Modifications Detected", "gpt_util -a <path> -J + --apply-from") \
	X(apply_write, "Apply with --write (Binary Roundtrip Fidelity)", "gpt_util export A + apply to B -> A == B") \
	/* Safety & Blocking (negative tests - expect operation to be blocked) */ \
	X(missing_section, "Safety - Missing GPT Section", "gpt_util export + remove section + apply (blocked)") \
	X(overlap_blocking, "Safety - Overlap Blocking", "gpt_util export overlaps + apply (blocked)") \
	X(mismatch_blocking, "Safety - Both Copies with Mismatch (blocked)", "gpt_util export both + apply (blocked)") \
	X(serial_number_mismatch, "Safety - Serial Number Mismatch Protection", "gpt_util export from A + apply to B (blocked)") \
	X(missing_serial_number, "Safety - Missing Serial Number Blocked", "gpt_util remove serial from JSON + apply (blocked)") \
	/* Delete Features */ \
	X(delete_main_entry, "Delete Main GPT Entry (_delete flag)", "gpt_util export + add _delete + apply --write") \
	X(delete_metadata_entry, "Delete Metadata GPT Entry (_delete in nested GPT)", "gpt_util export + delete metadata entry + apply --write") \
	/* Field Validation */ \
	X(readonly_fields_ignored, "Validation - _READONLY_ Fields Ignored", "gpt_util export + edit CRC + apply (CRC recalculated)") \
	X(static_fields_validated, "Validation - _STATIC_ Fields Validated", "gpt_util export + edit signature + apply (should succeed)") \
	X(nguid_preservation, "Validation - NGUID Preserved on Apply", "gpt_util apply without NGUID in JSON (NGUID unchanged)") \
	X(warning_fields_apply, "Validation - _WARNING_ Fields Applied", "gpt_util modify last_pba_zeroed + apply --write") \
	X(export_segment_metadata, "Validation - Segment Metadata Export", "gpt_util exports first 4K of each segment metadata partition") \
	/* Advanced Features */ \
	X(disk_metadata_apply, "disk_metadata Apply (safe fields)", "gpt_util export + edit disk_metadata + apply --write") \
	X(zero_change_write_skip, "Optimization - Skip Write When 0 Changes", "gpt_util apply identical JSON (no disk write)") \
	/* Binary Backup & Restore */ \
	X(binary_backup_restore, "Binary Backup & Restore", "gpt_util apply creates backup + restore works") \
	X(backup_restore_serial_mismatch, "Backup Safety - Serial Number Mismatch Blocked", "gpt_util restore to wrong device (serial mismatch)") \
	X(backup_restore_missing_file, "Backup Safety - Missing Structure File Blocked", "gpt_util restore with missing file") \
	X(backup_restore_corrupted_file, "Backup Safety - Corrupted File Blocked", "gpt_util restore with wrong file size") \
	X(backup_restore_incomplete_manifest, "Backup Safety - Incomplete Manifest Blocked", "gpt_util restore with missing manifest fields") \
	X(backup_restore_pba_overflow, "Backup Safety - PBA Overflow Blocked", "gpt_util restore from larger device to smaller") \
	X(backup_restore_block_size_mismatch, "Backup Safety - Block Size Mismatch Blocked", "gpt_util restore with wrong block size") \
	X(backup_restore_empty_structures, "Backup Safety - Empty Structures Array Blocked", "gpt_util restore with structures: []") \
	X(backup_creation_non_nvmesh_device, "Backup Failure - Non-NVMesh Device Blocked", "gpt_util backup device without metadata GPT") \
	X(restore_mid_failure_file_deleted, "Restore Mid-Failure - File Deleted During Restore", "gpt_util restore with file deleted mid-process") \
	/* Advanced JSON Modifications */ \
	X(json_add_partition_entry, "JSON Modification - Add Partition Entry", "gpt_util add new partition via JSON") \
	X(json_modify_metadata_gpt, "JSON Modification - Metadata GPT Changes", "gpt_util modify metadata partition name/range") \
	X(json_boundary_max_partitions, "Boundary Test - Max Partitions", "gpt_util handle max 8192 partitions") \
	X(stress_large_gpt_single_modification, "Stress Test - Single Entry Modification in Large GPT", "gpt_util modify 1 entry among 100+ partitions") \
	/* Edge Cases */ \
	X(csv_parsing_path, "Validation - CSV Parsing Path (-d)", "gpt_util -d with mock CSV (device discovery)") \
	/*X(malformed_json_type, "Safety - Malformed JSON Type Handling", "gpt_util apply with wrong JSON types (graceful failure)")*/ \
	X(o_direct_flags, "Validation - O_DIRECT Flags", "gpt_util --direct and --no-direct (I/O mode control)") \
	/* In-Memory GPT & TOMA Blocking */ \
	X(export_without_toma, "In-Memory GPT - Export Without TOMA", "gpt_util export when TOMA not running") \
	X(write_blocked_toma_running, "TOMA Safety - Write Blocked When TOMA Running", "gpt_util blocks writes if TOMA detected") \
	X(memory_sections_ignored, "In-Memory GPT - Memory Sections Ignored in Apply", "gpt_util apply ignores memory_* sections") \
	X(export_memory_gpt, "Export In-Memory GPT", "gpt_util export with mock memory GPT returning JSON")

// Define test function (searchable marker + function signature)
// Usage: DEFINE_TEST(normal_gpt) { test body }
#define DEFINE_TEST(name) \
	static int test_##name(struct self_test_ctx *ctx)

// Helper macros for test functions
#define TEST_JSON_PATH(name) TOMA_ROOT_DIR "tmp/test_" name ".json"

// Setup device or abort entire test suite (global macro for test functions)
#define SELF_TEST_SETUP_OR_ABORT(setup_func, path) \
	if (SELF_TEST_setup_device(setup_func, path) < 0) { \
		fprintf(stdout, COL_RED_BOLD "\nINFRASTRUCTURE FAILURE - Aborting test suite" COL_RESET "\n"); \
		exit(1); \
	}

// Setup test_argv from context (global macro for test functions)
#define SELF_TEST_ARGV(...) do { \
	const char *_args[] = {"gpt_util", __VA_ARGS__}; \
	int _i; \
	*ctx->test_argc = sizeof(_args) / sizeof(_args[0]); \
	for (_i = 0; _i < *ctx->test_argc; _i++) { \
		ctx->test_argv[_i] = (char *)_args[_i]; \
	} \
	optind = 1; \
} while(0)

/**
 * Run comprehensive self-test suite
 * @param test_selection: Comma-separated test numbers or ranges (e.g., "1,3-5,10" or NULL for all)
 * @param quiet_mode: If true, suppress decorative banners (only show results)
 * @return 0 on success
 */
int run_self_test(const char *test_selection, BOOL quiet_mode);

/**
 * Start a self-test case (SELF-TEST only)
 * Prints test header with the given test number
 * @param quiet_mode: If true, suppress decorative banners
 */
void SELF_TEST_start(int test_num, const char *description, const char *command, BOOL quiet_mode);

/**
 * Setup device for self-test (SELF-TEST only)
 * Calls the setup function, handles fd, prints status
 * Returns 0 on success, -1 on failure
 */
int SELF_TEST_setup_device(int (*setup_func)(const char *), const char *device_path);

/**
 * End a self-test case (SELF-TEST only)
 * Prints PASSED/FAILED based on result
 * Returns: 0 if passed, -1 if failed (for counting)
 */
int SELF_TEST_end(int test_num, int result);

/**
 * Generate a mock NVMesh disk with valid MBR and GPT structure for self-test
 * Serial number auto-generated from filepath (e.g., "MOCK-gpt_util_self_test")
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_and_open_mock_nvmesh_disk(const char *filepath);

/**
 * Generate a mock device with MODIFIED partition name for diff testing
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_mock_device_modified(const char *filepath);

/**
 * Generate a mock device with intentionally overlapping partitions
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_mock_device_with_overlaps(const char *filepath);

/**
 * Corrupt the alternate GPT on an already-written mock device for mismatch testing
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e);

/**
 * Write GPT header to disk at specified position (SELF-TEST ONLY)
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_write_gpt_header_at_position(int disk_fd, int pblk_size,
											const struct nvmeibt_disk_gpt_header *header,
											uint64_t header_pba);

/**
 * Corrupt Main GPT by setting n_partition_entries to a wrong value
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_corrupt_gpt_n_partition_entries(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e,
											   int wrong_n_partition_entries);

/**
 * Remove field from JSON file (SELF-TEST helper)
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_remove_json_field(const char *json_path, const char *field);

/**
 * Compare GPT binary data between two devices (SELF-TEST helper)
 * Returns 0 if identical, non-zero if different
 */
int SELF_TEST_compare_gpt_binary(const char *device_a, const char *device_b, int pblk_size, uint64_t n_blocks);

/**
 * Parse JSON file into key-value tree (gpt_util wrapper)
 * Returns the parsed tree (caller must free with nvmeibt_mm_json_free_kv_tree)
 */
struct mm_json_elem *SELF_TEST_parse_json_file(const char *filepath);

/**
 * Write JSON tree to file and free it (serializes, writes, frees)
 * Simplifies cleanup in tests - tree is always freed after write
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_write_json_file_and_free_kv_tree(struct mm_json_elem *json_root, const char *filepath);

/**
 * Validate boolean flag in exported JSON (SELF-TEST helper)
 * Returns 0 if flag matches expected value, -1 otherwise
 */
int SELF_TEST_validate_json_bool_flag(const char *json_path, const char *flag_name, BOOL expected_value);

#endif // #ifndef NVMEIBT_GPT_UTIL_SELF_TEST_H

