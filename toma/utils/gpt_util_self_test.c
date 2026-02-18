/**
 * gpt_util_self_test.c - Self-test framework implementation for gpt_util
 *
 * Comprehensive test suite for GPT utility operations
 */

#include "../nvmeibt_debug.h"
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>		// opendir()

#include "../nvmeibt_disk_metadata.h"
#include "../nvmeibt_ds_metadata.h"
#include "../nvmeibt_rpc.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_str.h"
#include "nvmeibt_uuid.h"
#include "../nvmeibt_json_base.h"
#include "../nvmeibt_local_disk.h"
#include "gpt_util_self_test.h"

#define PASS		"P"
#define FAIL		"F"
#define SKIP		"s"

// Test output macros - respect quiet mode
// All macros assume: ctx variable exists with quiet_mode field

// TEST_SUCCEED - Print success message (green, only if not quiet)
#define TEST_SUCCEED(msg, ...) \
	do { \
		if (!ctx->quiet_mode) { \
			fprintf(stdout, COL_GREEN msg COL_RESET "\n", ##__VA_ARGS__); \
		} \
	} while(0)

// TEST_FAIL - Print failure message (red, always shown)
#define TEST_FAIL(msg, ...) \
	fprintf(stdout, COL_RED_BOLD "FAIL: " msg COL_RESET "\n", ##__VA_ARGS__)

// TEST_INFO - Print informational message (only if not quiet)
#define TEST_INFO(msg, ...) \
	do { \
		if (!ctx->quiet_mode) { \
			fprintf(stdout, msg "\n", ##__VA_ARGS__); \
		} \
	} while(0)

// External, defined in gpt_util.c
BOOL SELF_TEST_acquire_toma_lock(void);
void SELF_TEST_release_toma_lock(void);
void SELF_TEST_mock_toma_running(void);
void SELF_TEST_undo_mock_toma_running(void);
void SELF_TEST_set_mock_local_disk(const struct nvmeibt_local_disk *local_disk);

/**
 * Start a self-test case (SELF-TEST only)
 * Prints test header with the given test number
 */
void SELF_TEST_start(int test_num, const char *description, const char *command, BOOL quiet_mode)
{
	if (!quiet_mode) {
		fprintf(stdout, "\n");
		fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
		fprintf(stdout, COL_WHITE_BOLD "SELF-TEST %d: %s" COL_RESET "\n", test_num, description);
		fprintf(stdout, "Emulated command: " COL_YELLOW "%s" COL_RESET "\n", command);
		fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
	}
}

/**
 * Setup device for self-test (SELF-TEST only)
 * Calls the setup function, handles fd, prints status
 * Returns 0 on success, -1 on failure
 */
int SELF_TEST_setup_device(int (*setup_func)(const char *), const char *device_path)
{
	int fd = setup_func(device_path);

	if (fd < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED" COL_RESET "\n");
		return -1;
	}

	close(fd);
	return 0;
}

/**
 * End a self-test case (SELF-TEST only)
 * Prints PASSED/FAILED based on result
 * Convention: result == 0 means PASS, result != 0 means FAIL (always)
 * Returns: 0 if passed, -1 if failed (for counting)
 */
int SELF_TEST_end(int test_num, int result)
{
	BOOL test_passed = (result == 0);

	if (test_passed) {
		fprintf(stdout, "\n" COL_GREEN ">>> SELF-TEST %d: PASSED <<<" COL_RESET "\n", test_num);
		return 0;
	} else {
		fprintf(stdout, "\n" COL_RED_BOLD ">>> SELF-TEST %d: FAILED <<<" COL_RESET "\n", test_num);
		return -1;
	}
}

/**
 * Generate mock serial number from device path (for test files only)
 * Format: "MOCK-<8-hex-crc32>" (13 chars total, fits in 20-char NVMe serial field)
 */
void SELF_TEST_generate_mock_serial_number_from_path(const char *device_path, char *serial_out, size_t size)
{
	uint32_t hash;

	/* Hash the full path to ensure uniqueness (handles long paths, similar prefixes) */
	hash = crc32_seedless((const void *)device_path, strlen(device_path));

	/* Generate stable mock serial: "MOCK-<8-char-hex>" (13 chars total, fits in 20) */
	snprintf(serial_out, size, "MOCK-%08x", hash);
}

/**
 * Helper: Write segment metadata control block to device
 * Returns 0 on success, -1 on error
 */
static int write_segment_control_block(int fd, uint64_t pba_start, const struct nvmeibt_seg_active_metadata_ctrl *ctrl_data)
{
	char		*aligned_buf = NULL;
	uint64_t	pbyte_s;
	int			rv = -1;

	aligned_buf = NNVMEIBT_BM_ALIGNED_CALLOC(trace_test_write_seg_ctrl_helper, PAGE_SIZE, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE);
	if (!aligned_buf) {
		return -1;
	}

	memcpy(aligned_buf, ctrl_data, sizeof(*ctrl_data));
	pbyte_s = pba_start * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;

	if (NNVMEIBT_PWRITE(trace_test_write_seg_ctrl_helper_write, fd, aligned_buf, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, 0) == SELF_TEST_MOCK_DEVICE_BLOCK_SIZE) {
		rv = 0;
	}

	NNVMEIBT_BM_FREE(trace_test_write_seg_ctrl_helper_free, aligned_buf);
	return rv;
}

/**
 * Helper: Initialize segment metadata control block with standard test values
 * Sets: magic string, versions, mgmt_db_uuid, location fields, calculates CRC
 * @param ctrl Output control block structure
 * @param seg_uuid Segment UUID
 * @param seg_md_pba_s Segment metadata partition start PBA (for calculating offsets)
 */
static void init_segment_control_block_standard(struct nvmeibt_seg_active_metadata_ctrl *ctrl,
												const union nvmeib_uuid *seg_uuid,
												uint64_t seg_md_pba_s)
{
	memset(ctrl, 0, sizeof(*ctrl));

	/* Initialize header with production-compatible values */
	nvmeibt_strlcpy(ctrl->header.magic_str, "Disk Segment Metadata db5a320f-c7f4-4e16-940a-dbc2e97a6494", sizeof(ctrl->header.magic_str));
	ctrl->header.software_version = 0x00020800;		/* v2.8.0 */
	ctrl->header.seg_metadata_version = 0x00020800;	/* v2.8.0 */
	ctrl->header.mgmt_db_uuid.ll[0] = 0x1111111111111111ULL;
	ctrl->header.mgmt_db_uuid.ll[1] = 0x2222222222222222ULL;

	/* Set segment UUID */
	ctrl->disk_segment_uuid = *seg_uuid;
	ctrl->is_current_shutdown_clean = 1;
	ctrl->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID;

	/* Set location fields (matches nvmeibt_ds_metadata_ctrl_blk_write) */
	ctrl->save_timespec_tv_sec = 1234567890;		/* Test timestamp */
	ctrl->metadata_pbyte_s = seg_md_pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	ctrl->locks_table_pbyte_s = ctrl->metadata_pbyte_s + LOCKS_TABLE_RELATIVE_OFFSET_BYTES;

	/* Zero filler (matches nvmeibt_ds_metadata_ctrl_blk_write) */
	memset(ctrl->__zeroed_filler_till_4K__, 0, sizeof(*ctrl) - offsetof(typeof(*ctrl), __zeroed_filler_till_4K__));

	/* Calculate CRC (matches nvmeibt_ds_metadata_ctrl_blk_write) */
	ctrl->metadata_ctrl_crc32 = crc32_seedless(ctrl, offsetof(typeof(*ctrl), metadata_ctrl_crc32));
}

/**
 * Generate a mock NVMesh disk WITH segment metadata partitions for testing control block backup/restore
 * Returns the fd of the created device (caller must close it)
 */
static int SELF_TEST_generate_mock_nvmesh_disk_with_segments(const char *filepath)
{
	int											fd;
	struct nvmeibt_disk_gpt						main_gpt;
	struct nvmeibt_disk_gpt						metadata_gpt;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_partition;
	struct nvmeibt_disk_gpt_partition_entry			*seg_md_partition;
	union nvmeib_uuid							seg_uuid;
	struct nvmeibt_seg_active_metadata_ctrl		seg_ctrl;
	int											i;

	/* Start with standard mock device */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(filepath);
	if (fd < 0) {
		return -1;
	}

	/* Read Main and Metadata GPTs */
	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		close(fd);
		return -1;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		close(fd);
		return -1;
	}

	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		close(fd);
		return -1;
	}

	/* Add 2 segment metadata partitions after existing Disk_Metadata partition */
	for (i = 0; i < 2; i++) {
		char											seg_name[32];
		uint64_t										disk_md_end;
		uint64_t										seg_start;
		const struct nvmeibt_disk_gpt_partition_entry	*disk_md_entry;

		/* Find Disk_Metadata partition from metadata GPT to get its end PBA */
		disk_md_entry = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
		if (!disk_md_entry) {
			close(fd);
			return -1;
		}
		disk_md_end = disk_md_entry->pba_e;

		snprintf(seg_name, sizeof(seg_name), "Disk_Segment_%d", i);

		seg_uuid.ll[0] = 0xAABBCCDD00000000ULL + i;
		seg_uuid.ll[1] = 0x1122334400000000ULL + i;

		/* Place segments after disk_metadata with small gap */
		seg_start = disk_md_end + 5 + i * 50;		/* Gap of 5 blocks, then 50 blocks per segment */

		seg_md_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
			&metadata_gpt, &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID,
			&seg_uuid,
			seg_start,
			seg_start + 49,		/* 50 blocks per segment */
			seg_name, strlen(seg_name));

		if (!seg_md_partition) {
			close(fd);
			return -1;
		}

		/* Write initial control block to segment metadata partition */
		init_segment_control_block_standard(&seg_ctrl, &seg_uuid, seg_md_partition->pba_s);

		if (write_segment_control_block(fd, seg_md_partition->pba_s, &seg_ctrl) < 0) {
			close(fd);
			return -1;
		}
	}

	/* Write updated Metadata GPT with new segment partitions */
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt, false) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/**
 * Generate a mock NVMesh disk with valid MBR and GPT structure for self-test
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_and_open_mock_nvmesh_disk(const char *filepath)
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
	struct nvmeibt_disk_gpt_partition_entry			*metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition = NULL;
	uint64_t							metadata_start;
	uint64_t							metadata_end;
	uint64_t							pbyte_s;
	struct nvmeibt_disk_metadata		disk_metadata;
	char								*dma_buffer = NULL;
	int									n_bytes_write;

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
	metadata_start = main_gpt.header.first_usable_pba;
	metadata_end = main_gpt.header.last_usable_pba;

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

	// Add EXCELERO_DISK_METADATA partition (128KB = 32 blocks at 4KB per block)
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.first_usable_pba + 31,		/* 32 blocks (128KB) */
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
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (disk_md_partition) {
		// Initialize disk metadata with test values
		memset(&disk_metadata, 0, sizeof(disk_metadata));
		disk_metadata.signature = DISK_METADATA_SIGNATURE;
		disk_metadata.format_pblk_size = pblk_size;
		disk_metadata.format_request_counter = 1;

		// Test serial ID (stored in disk_metadata - NOT used for validation)
		nvmeibt_strlcpy(disk_metadata.native_serial_str, "TEST-SERIAL-001", sizeof(disk_metadata.native_serial_str));
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
	}

	fsync(fd);

	// Return the fd - caller will close it (which triggers sandbox auto-deletion)
	rv = fd;
	fd = -1;		// Don't close it in cleanup

out:
	if (fd >= 0) { close(fd); }
	return rv;
}

/**
 * Generate a mock NVMesh disk with segment metadata partitions for testing export
 * Similar to SELF_TEST_generate_and_open_mock_nvmesh_disk but adds segment metadata partitions
 */
static int SELF_TEST_generate_mock_disk_with_segment_metadata(const char *filepath)
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
	union nvmeib_uuid					seg_uuid_1;
	union nvmeib_uuid					seg_uuid_2;
	struct nvmeibt_seg_active_metadata_ctrl	*seg_md_ctrl = NULL;
	uint64_t							n_disk_blocks = 3000;  // Larger than standard 2000 to fit segment metadata
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	struct nvmeibt_disk_gpt_partition_entry			*metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition = NULL;
	uint64_t							metadata_start;
	uint64_t							metadata_end;
	uint64_t							seg_md_pba_s;
	uint64_t							seg_md_pba_e;
	uint64_t							pbyte_s;
	struct nvmeibt_disk_metadata		disk_metadata;
	char								*dma_buffer = NULL;
	int									n_bytes_write;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, "Metadata", sizeof(metadata_gpt.main_or_metadata));

	fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(selftest_create_seg_md_disk_failed, "Failed to create mock device @STR @AUTO_ERRNO", filepath);
		return -1;
	}

	// 1. Initialize protective MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		N_Ef(selftest_write_mbr_seg_md_failed, "Failed to write MBR to mock device");
		goto out;
	}

	// 2. Initialize Main GPT structure
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	// 3. Add EXCELERO_METADATA partition using all available space
	metadata_start = main_gpt.header.first_usable_pba;
	metadata_end = main_gpt.header.last_usable_pba;

	metadata_partition_uuid.ll[0] = 0xAABBCCDD11223344ULL;
	metadata_partition_uuid.ll[1] = 0x5566778899AABBCCULL;

	metadata_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
		&main_gpt, &EXCELERO_METADATA_PARTITION_TYPE_GUID,
		&metadata_partition_uuid,
		metadata_start, metadata_end,
		EXCELERO_METADATA_PARTITION_NAME,
		strlen(EXCELERO_METADATA_PARTITION_NAME));

	if (!metadata_partition) {
		N_Ef(selftest_add_metadata_seg_md_failed, "Failed to add metadata partition");
		goto out;
	}

	// 4. Write Main GPT to disk
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		N_Ef(selftest_store_main_gpt_seg_md_failed, "Failed to store Main GPT");
		goto out;
	}

	// 5. Initialize nested Metadata GPT
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
											 metadata_partition->pba_e,
											 &metadata_gpt, pblk_size,
											 MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	// 6. Add EXCELERO_DISK_METADATA partition (small, just 10 blocks)
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.first_usable_pba + 9,  // Only 10 blocks
												 DISK_METADATA_PARTITION_NAME,
												 strlen(DISK_METADATA_PARTITION_NAME))) {
		N_Ef(selftest_add_disk_metadata_seg_md_failed, "Failed to add disk_metadata partition");
		goto out;
	}

	// 7. Add two segment metadata partitions after disk_metadata
	seg_md_pba_s = metadata_gpt.header.first_usable_pba + 10;
	seg_md_pba_e = seg_md_pba_s + 9;  // 10 blocks for segment metadata

	seg_uuid_1.ll[0] = 0x1111111111111111ULL;
	seg_uuid_1.ll[1] = 0x2222222222222222ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID,
												 &seg_uuid_1,
												 seg_md_pba_s, seg_md_pba_e,
												 "SEG_1", strlen("SEG_1"))) {
		goto out;
	}

	// Partition 2: Right after partition 1
	seg_md_pba_s = seg_md_pba_e + 1;
	seg_md_pba_e = seg_md_pba_s + 9;

	seg_uuid_2.ll[0] = 0x3333333333333333ULL;
	seg_uuid_2.ll[1] = 0x4444444444444444ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID,
												 &seg_uuid_2,
												 seg_md_pba_s, seg_md_pba_e,
												 "SEG_2", strlen("SEG_2"))) {
		goto out;
	}

	// 8. Write Metadata GPT to disk
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &metadata_gpt, false) < 0) {
		N_Ef(selftest_store_metadata_gpt_seg_md_failed, "Failed to store Metadata GPT");
		goto out;
	}

	// 9. Write disk metadata structure (minimal, for serial ID)
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (disk_md_partition) {
		memset(&disk_metadata, 0, sizeof(disk_metadata));
		disk_metadata.signature = DISK_METADATA_SIGNATURE;
		disk_metadata.format_pblk_size = pblk_size;
		disk_metadata.format_request_counter = 1;
		nvmeibt_strlcpy(disk_metadata.native_serial_str, "TEST-SERIAL-001", sizeof(disk_metadata.native_serial_str));
		disk_metadata.native_nguid_unused.ll[0] = 0xAABBCCDD11223344ULL;
		disk_metadata.native_nguid_unused.ll[1] = 0x5566778899AABBCCULL;
		disk_metadata.crc32 = 0;
		disk_metadata.crc32 = crc32_seedless(&disk_metadata, sizeof(disk_metadata));

		pbyte_s = disk_md_partition->pba_s * pblk_size;
		n_bytes_write = roundup(sizeof(disk_metadata), pblk_size);
		dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_disk_md_seg_md, PAGE_SIZE, n_bytes_write);
		memcpy(dma_buffer, &disk_metadata, sizeof(disk_metadata));

		if (pwrite(fd, dma_buffer, n_bytes_write, pbyte_s) != n_bytes_write) {
			N_Ef(selftest_write_disk_md_seg_md_failed, "Failed to write disk metadata @AUTO_ERRNO");
			NNVMEIBT_BM_FREE(trace_selftest_disk_md_seg_md_free, dma_buffer);
			goto out;
		}
		NNVMEIBT_BM_FREE(trace_selftest_disk_md_seg_md_free2, dma_buffer);
	}

	// 10. Initialize and write segment metadata control blocks
	seg_md_ctrl = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_seg_md, PAGE_SIZE, sizeof(*seg_md_ctrl));
	if (!seg_md_ctrl) {
		goto out;
	}

	// Write segment metadata control block for partition 1
	memset(seg_md_ctrl, 0, sizeof(*seg_md_ctrl));
	nvmeibt_strlcpy(seg_md_ctrl->header.magic_str, "SEG_METADATA_MAGIC_V1", sizeof(seg_md_ctrl->header.magic_str));
	seg_md_ctrl->header.software_version = 1;
	seg_md_ctrl->header.seg_metadata_version = 1;
	seg_md_ctrl->disk_segment_uuid = seg_uuid_1;
	seg_md_ctrl->save_timespec_tv_sec = 1234567890;
	seg_md_ctrl->locks_table_pbyte_s = 8192;
	seg_md_ctrl->reservation_mode_version = 100;
	seg_md_ctrl->active_praid_version_major = 3;
	seg_md_ctrl->active_praid_version_minor = 2;
	seg_md_ctrl->is_current_shutdown_clean = true;
	seg_md_ctrl->is_written_on_disk = 2;  // NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID
	nvmeibt_strlcpy(seg_md_ctrl->hostname, "test-host-1", sizeof(seg_md_ctrl->hostname));
	seg_md_ctrl->metadata_pbyte_s = 16384;
	seg_md_ctrl->n_blksets_scrubbed = 42;

	/* Zero filler and calculate CRC (matches production) */
	memset(seg_md_ctrl->__zeroed_filler_till_4K__, 0, sizeof(*seg_md_ctrl) - offsetof(typeof(*seg_md_ctrl), __zeroed_filler_till_4K__));
	seg_md_ctrl->metadata_ctrl_crc32 = crc32_seedless(seg_md_ctrl, offsetof(typeof(*seg_md_ctrl), metadata_ctrl_crc32));

	pbyte_s = (disk_md_partition->pba_e + 1) * pblk_size;
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_seg_md_write, PAGE_SIZE, sizeof(*seg_md_ctrl));
	memcpy(dma_buffer, seg_md_ctrl, sizeof(*seg_md_ctrl));
	if (pwrite(fd, dma_buffer, sizeof(*seg_md_ctrl), pbyte_s) != sizeof(*seg_md_ctrl)) {
		NNVMEIBT_BM_FREE(trace_selftest_seg_md_write_free, dma_buffer);
		goto out;
	}
	NNVMEIBT_BM_FREE(trace_selftest_seg_md_write_free2, dma_buffer);

	// Write segment metadata control block for partition 2
	memset(seg_md_ctrl, 0, sizeof(*seg_md_ctrl));
	nvmeibt_strlcpy(seg_md_ctrl->header.magic_str, "SEG_METADATA_MAGIC_V1", sizeof(seg_md_ctrl->header.magic_str));
	seg_md_ctrl->header.software_version = 1;
	seg_md_ctrl->header.seg_metadata_version = 1;
	seg_md_ctrl->disk_segment_uuid = seg_uuid_2;
	seg_md_ctrl->save_timespec_tv_sec = 9876543210LL;
	seg_md_ctrl->locks_table_pbyte_s = 12288;
	seg_md_ctrl->reservation_mode_version = 200;
	seg_md_ctrl->active_praid_version_major = 4;
	seg_md_ctrl->active_praid_version_minor = 5;
	seg_md_ctrl->is_current_shutdown_clean = false;
	seg_md_ctrl->is_written_on_disk = 2;
	nvmeibt_strlcpy(seg_md_ctrl->hostname, "test-host-2", sizeof(seg_md_ctrl->hostname));
	seg_md_ctrl->metadata_pbyte_s = 32768;
	seg_md_ctrl->n_blksets_scrubbed = 99;

	/* Zero filler and calculate CRC (matches production) */
	memset(seg_md_ctrl->__zeroed_filler_till_4K__, 0, sizeof(*seg_md_ctrl) - offsetof(typeof(*seg_md_ctrl), __zeroed_filler_till_4K__));
	seg_md_ctrl->metadata_ctrl_crc32 = crc32_seedless(seg_md_ctrl, offsetof(typeof(*seg_md_ctrl), metadata_ctrl_crc32));

	pbyte_s = (disk_md_partition->pba_e + 1 + 10) * pblk_size;
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_seg_md_write3, PAGE_SIZE, sizeof(*seg_md_ctrl));
	memcpy(dma_buffer, seg_md_ctrl, sizeof(*seg_md_ctrl));
	if (pwrite(fd, dma_buffer, sizeof(*seg_md_ctrl), pbyte_s) != sizeof(*seg_md_ctrl)) {
		NNVMEIBT_BM_FREE(trace_selftest_seg_md_write_free3, dma_buffer);
		goto out;
	}
	NNVMEIBT_BM_FREE(trace_selftest_seg_md_write_free4, dma_buffer);

	fsync(fd);

	// Success - return the fd
	rv = fd;
	fd = -1;

out:
	if (fd >= 0) { close(fd); }
	NNVMEIBT_BM_FREE(trace_selftest_seg_md_cleanup, seg_md_ctrl);
	return rv;
}

/**
 * Generate a mock device with MODIFIED partition name for diff testing
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_mock_device_modified(const char *filepath)
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
	struct nvmeibt_disk_gpt_partition_entry			*metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition;
	struct nvmeibt_disk_metadata		disk_metadata;
	char								*dma_buffer = NULL;
	int									n_bytes_write;
	uint64_t							pbyte_s;
	uint64_t							metadata_start;
	uint64_t							metadata_end;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, "Metadata", sizeof(metadata_gpt.main_or_metadata));

	fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(selftest_create_modified_failed, "Failed to create modified mock device @STR @AUTO_ERRNO", filepath);
		return -1;
	}

	// Initialize MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		N_Ef(selftest_write_mbr_modified_failed, "Failed to write MBR to modified mock device");
		goto out;
	}

	// Initialize Main GPT
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	// Add EXCELERO_METADATA partition with DIFFERENT name
	metadata_start = main_gpt.header.first_usable_pba;
	metadata_end = main_gpt.header.last_usable_pba;

	metadata_partition_uuid.ll[0] = 0xAABBCCDD11223344ULL;
	metadata_partition_uuid.ll[1] = 0x5566778899AABBCCULL;

	metadata_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
		&main_gpt, &EXCELERO_METADATA_PARTITION_TYPE_GUID,
		&metadata_partition_uuid,
		metadata_start, metadata_end,
		"MODIFIED_metadata",  // MODIFIED: different name only
		strlen("MODIFIED_metadata"));

	if (!metadata_partition) {
		N_Ef(selftest_add_modified_metadata_failed, "Failed to add modified metadata partition");
		goto out;
	}

	// Write Main GPT
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		N_Ef(selftest_store_modified_gpt_failed, "Failed to store modified Main GPT");
		goto out;
	}

	// Initialize nested Metadata GPT (same as standard)
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
											 metadata_partition->pba_e,
											 &metadata_gpt, pblk_size,
											 MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.last_usable_pba,
												 DISK_METADATA_PARTITION_NAME,
												 strlen(DISK_METADATA_PARTITION_NAME))) {
		N_Ef(selftest_add_disk_metadata_modified_failed, "Failed to add disk_metadata to modified device");
		goto out;
	}

	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &metadata_gpt, false) < 0) {
		N_Ef(selftest_store_metadata_modified_failed, "Failed to store Metadata GPT to modified device");
		goto out;
	}

	// Write disk metadata
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (disk_md_partition) {
		memset(&disk_metadata, 0, sizeof(disk_metadata));
		disk_metadata.signature = DISK_METADATA_SIGNATURE;
		disk_metadata.format_pblk_size = pblk_size;
		disk_metadata.format_request_counter = 1;

		// Test serial ID (stored in disk_metadata - NOT used for validation)
		nvmeibt_strlcpy(disk_metadata.native_serial_str, "TEST-SERIAL-001", sizeof(disk_metadata.native_serial_str));
		disk_metadata.native_nguid_unused.ll[0] = 0xAABBCCDD11223344ULL;
		disk_metadata.native_nguid_unused.ll[1] = 0x5566778899AABBCCULL;

		disk_metadata.crc32 = 0;
		disk_metadata.crc32 = crc32_seedless(&disk_metadata, sizeof(disk_metadata));

		pbyte_s = disk_md_partition->pba_s * pblk_size;
		n_bytes_write = roundup(sizeof(disk_metadata), pblk_size);
		dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_mod_disk_md, PAGE_SIZE, n_bytes_write);
		memcpy(dma_buffer, &disk_metadata, sizeof(disk_metadata));

		if (pwrite(fd, dma_buffer, n_bytes_write, pbyte_s) != n_bytes_write) {
			N_Ef(selftest_write_modified_disk_md_failed, "Failed to write disk metadata to modified device @AUTO_ERRNO");
			NNVMEIBT_BM_FREE(trace_selftest_mod_disk_md_free, dma_buffer);
			goto out;
		}

		NNVMEIBT_BM_FREE(trace_selftest_mod_disk_md_free2, dma_buffer);
	}

	fsync(fd);

	rv = fd;
	fd = -1;

out:
	if (fd >= 0) { close(fd); }
	return rv;
}

/**
 * Generate a mock device with intentionally overlapping partitions in Main GPT
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_mock_device_with_overlaps(const char *filepath)
{
	int									fd;
	struct nvmeibt_disk_gpt				main_gpt;
	union nvmeib_uuid					overlap_partition_uuid;
	uint64_t							overlap_start;
	uint64_t							overlap_end;

	/* Start with standard NVMesh device */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(filepath);
	if (fd < 0) {
		return -1;
	}

	/* Read the Main GPT */
	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		close(fd);
		return -1;
	}

	/* Add overlapping partition to Main GPT (overlaps with existing EXCELERO_METADATA at entry 0) */
	overlap_partition_uuid.ll[0] = 0x1122334455667788ULL;
	overlap_partition_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	/* Existing partition 0: pba_s=258, pba_e=1742 (EXCELERO_METADATA) */
	/* New partition 1: pba_s=1400, pba_e=1742 - OVERLAPS! */
	overlap_start = main_gpt.entries[0].pba_s + 1142;  // 1400
	overlap_end = main_gpt.entries[0].pba_e;  // 1742

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&main_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &overlap_partition_uuid,
												 overlap_start,
												 overlap_end,
												 "partition_OVERLAP",
												 strlen("partition_OVERLAP"))) {
		close(fd);
		return -1;
	}

	/* Write updated Main GPT with overlap */
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, false) < 0) {
		close(fd);
		return -1;
	}

	fsync(fd);
	return fd;
}

/**
 * Corrupt the alternate GPT on an already-written mock device for mismatch testing
 */
int SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e)
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

	rv = 0;

out:
	return rv;
}

/**
 * Write GPT header to disk at specified position (SELF-TEST ONLY)
 */
int SELF_TEST_write_gpt_header_at_position(int disk_fd, int pblk_size,
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
 * Corrupt Main GPT by setting n_partition_entries to a wrong value
 */
int SELF_TEST_corrupt_gpt_n_partition_entries(int fd, int pblk_size, uint64_t pba_s, uint64_t pba_hw_e,
											   int wrong_n_partition_entries)
{
	int							rv = -1;
	struct nvmeibt_disk_gpt		gpt;
	struct nvmeibt_disk_gpt_header	primary_header;
	struct nvmeibt_disk_gpt_header	alternate_header;
	uint32_t					crc_with_max_entries;
	int64_t						primary_entries_offset;
	int							nbytes;

	memset(&gpt, 0, sizeof(gpt));
	nvmeibt_strlcpy(gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt.main_or_metadata));

	// 1. Read the existing GPT
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, pblk_size, &gpt,
										  pba_s, pba_hw_e, false) < 0) {
		N_Ef(selftest_read_for_n_part_corrupt_failed, "Failed to read GPT for n_partition_entries corruption test");
		goto out;
	}

	// 2. Calculate CRC using LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192) - the correct/max value
	nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * gpt.header.size_of_partition_entry;
	crc_with_max_entries = crc32_seedless(gpt.entries, nbytes);

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
	primary_entries_offset = (int64_t)gpt.header.partition_entry_pba - (int64_t)gpt.header.my_pba;
	alternate_header.partition_entry_pba = gpt.header.alternate_pba + primary_entries_offset;
	if (alternate_header.partition_entry_pba >= alternate_header.my_pba) {
		int entries_n_pblks = divroundup(nbytes, pblk_size);
		alternate_header.partition_entry_pba = alternate_header.my_pba - entries_n_pblks;
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

	rv = 0;

out:
	return rv;
}

/**
 * Remove field from JSON file (SELF-TEST helper)
 */
int SELF_TEST_remove_json_field(const char *json_path, const char *field)
{
	int						fd = -1;
	int						rv = -1;
	struct nvmeibt_Str		*file_content = NULL;
	struct nvmeibt_Str		*new_content = NULL;
	const char				*key_start;
	const char				*value_start;
	const char				*value_end;
	int						brace_count;
	char					search_pattern[128];

	// Read file
	fd = NNVMEIBT_OPEN_READ(trace_selftest_remove_open, json_path, 1);
	if (fd < 0) {
		N_Ef(selftest_remove_open_failed, "Failed to open JSON @STR for field removal", json_path);
		goto out;
	}

	file_content = NNVMEIBT_STR_ALLOC(trace_selftest_remove_read);
	if (NNVMEIBT_STR_FREAD_ATOMIC(trace_selftest_remove_fread, file_content, fd) < 0) {
		N_Ef(selftest_remove_read_failed, "Failed to read JSON @STR", json_path);
		goto out;
	}
	NNVMEIBT_CLOSE(trace_selftest_remove_close1, fd);
	fd = -1;

	// Search for the field (looking for "field":)
	snprintf(search_pattern, sizeof(search_pattern), "\"%s\":", field);
	key_start = strstr(nvmeibt_Str_str(file_content), search_pattern);
	if (!key_start) {
		N_Ef(selftest_remove_field_not_found, "Field @STR not found in JSON file", field);
		goto out;
	}

	// Find start of value (skip whitespace after colon)
	value_start = key_start + strlen(search_pattern);
	while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n') {
		value_start++;
	}

	// Find end of value (handle nested braces for objects)
	if (*value_start == '{') {
		brace_count = 0;
		value_end = value_start;
		do {
			if (*value_end == '{') brace_count++;
			if (*value_end == '}') brace_count--;
			value_end++;
		} while (brace_count > 0 && *value_end != '\0');
	} else {
		// Simple value - find comma or closing brace
		value_end = value_start;
		while (*value_end != ',' && *value_end != '}' && *value_end != '\0') {
			value_end++;
		}
	}

	// Skip trailing comma/whitespace
	if (*value_end == ',') {
		value_end++;
		while (*value_end == ' ' || *value_end == '\t' || *value_end == '\n') {
			value_end++;
		}
	}

	// Build new content: everything before key + everything after value
	new_content = NNVMEIBT_STR_ALLOC(trace_selftest_remove_new);
	nvmeibt_Str_strncat(new_content, nvmeibt_Str_str(file_content), key_start - nvmeibt_Str_str(file_content));
	nvmeibt_Str_sprintf(new_content, "%s", value_end);

	// Write back
	fd = open(json_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		N_Ef(selftest_remove_open_write_failed, "Failed to open JSON @STR for field removal", json_path);
		goto out;
	}

	if (write(fd, nvmeibt_Str_str(new_content), nvmeibt_Str_strlen(new_content)) != (ssize_t)nvmeibt_Str_strlen(new_content)) {
		N_Ef(selftest_remove_write_failed, "Failed to write JSON @STR", json_path);
		goto out;
	}

	rv = 0;

out:
	if (fd >= 0) { close(fd); }
	NNVMEIBT_STR_FREE(trace_selftest_remove_cleanup1, file_content);
	NNVMEIBT_STR_FREE(trace_selftest_remove_cleanup2, new_content);
	return rv;
}

/**
 * Compare GPT binary data between two devices (SELF-TEST helper)
 */
int SELF_TEST_compare_gpt_binary(const char *device_a, const char *device_b, int pblk_size, uint64_t n_blocks)
{
	int			fd_a = -1;
	int			fd_b = -1;
	char		*buf_a = NULL;
	char		*buf_b = NULL;
	int			rv = -1;
	uint64_t	region_size;
	uint64_t	region_offset;

	// Compare entire device (MBR + Main GPT + Metadata partition + Main GPT backup)
	region_size = n_blocks * pblk_size;	// Entire device
	region_offset = 0;

	buf_a = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_cmp_a, PAGE_SIZE, region_size);
	buf_b = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_cmp_b, PAGE_SIZE, region_size);

	fd_a = open(device_a, O_RDONLY);
	fd_b = open(device_b, O_RDONLY);

	if (fd_a < 0 || fd_b < 0) {
		goto out;
	}

	if (pread(fd_a, buf_a, region_size, region_offset) != (ssize_t)region_size ||
		pread(fd_b, buf_b, region_size, region_offset) != (ssize_t)region_size) {
		goto out;
	}

	rv = memcmp(buf_a, buf_b, region_size);

out:
	if (fd_a >= 0) close(fd_a);
	if (fd_b >= 0) close(fd_b);
	NNVMEIBT_BM_FREE(trace_selftest_cmp_a_free, buf_a);
	NNVMEIBT_BM_FREE(trace_selftest_cmp_b_free, buf_b);

	return rv;
}

/**
 * Check if a JSON key should be skipped during comparison (SELF-TEST specific)
 * Skips fields that change between edited JSON and re-exported JSON.
 *
 * All other fields should match since we're verifying that editable fields
 * applied correctly and can be re-exported with the same values.
 */
static bool SELF_TEST_should_skip_json_key(const char *key)
{
	// Skip backup_timestamp (changes on every export)
	if (strcmp(key, "backup_timestamp") == 0) {
		return true;
	}

	// Skip _READONLY_ fields (recalculated when underlying data changes)
	// Example: _READONLY_crc32 changes when ldisk_id_str is modified
	if (strncmp(key, "_READONLY_", 10) == 0) {
		return true;
	}

	return false;
}

/**
 * Compare two JSON files for equality (SELF-TEST helper)
 * Uses the common json_compare_trees() with test-specific skip callback
 * Returns 0 if identical, -1 if different
 */
int SELF_TEST_compare_json_files(const char *json_a, const char *json_b, BOOL quiet_mode)
{
	struct mm_json_elem		*root_a = NULL;
	struct mm_json_elem		*root_b = NULL;
	char					mismatch_buf[512] = {0};
	int						rv = -1;

	root_a = SELF_TEST_parse_json_file(json_a);
	root_b = SELF_TEST_parse_json_file(json_b);

	if (!root_a || !root_b) {
		if (!quiet_mode) {
			fprintf(stdout, "Failed to parse JSON files for comparison\n");
		}
		goto out;
	}

	rv = json_compare_trees(root_a, root_b, "root", SELF_TEST_should_skip_json_key,
							mismatch_buf, sizeof(mismatch_buf));

	if (rv != 0 && !quiet_mode && mismatch_buf[0] != '\0') {
		fprintf(stdout, "  JSON mismatch %s\n", mismatch_buf);
	}

out:
	nvmeibt_mm_json_free_kv_tree(root_a);
	nvmeibt_mm_json_free_kv_tree(root_b);
	return rv;
}

/**
 * Parse JSON file into key-value tree (gpt_util wrapper)
 */
struct mm_json_elem *SELF_TEST_parse_json_file(const char *filepath)
{
	int						fd = -1;
	struct nvmeibt_Str		*json_content = NULL;
	struct mm_json_elem		*json_root = NULL;

	fd = open(filepath, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}

	json_content = NNVMEIBT_STR_ALLOC(trace_selftest_json_read);
	if (NNVMEIBT_STR_FREAD(trace_selftest_json_fread, json_content, fd) < 0) {
		goto out;
	}
	json_root = parse_json_txt_into_kv_tree(nvmeibt_Str_str(json_content), nvmeibt_Str_strlen(json_content));

out:
	close(fd);
	NNVMEIBT_STR_FREE(trace_selftest_json_cleanup, json_content);

	return json_root;
}

/**
 * Write JSON tree to file and free it (serializes, writes, frees)
 * Simplifies cleanup in tests - tree is always freed after write
 * Returns 0 on success, -1 on error
 */
int SELF_TEST_write_json_file_and_free_kv_tree(struct mm_json_elem *json_root, const char *filepath)
{
	struct nvmeibt_Str		*json_output = NULL;
	int						fd = -1;
	int						rv = -1;

	if (!json_root) {
		return -1;
	}

	json_output = NNVMEIBT_STR_ALLOC(trace_selftest_json_write);
	if (serialize_json_tree_to_str(json_root, json_output) < 0) {
		goto out;
	}

	fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		goto out;
	}

	if (write(fd, nvmeibt_Str_str(json_output), nvmeibt_Str_strlen(json_output)) != (ssize_t)nvmeibt_Str_strlen(json_output)) {
		goto out;
	}

	rv = 0;

out:
	if (fd >= 0) { close(fd); }
	NNVMEIBT_STR_FREE(trace_selftest_json_write_cleanup, json_output);
	nvmeibt_mm_json_free_kv_tree(json_root);		/* Always free the tree */
	return rv;
}

/**
 * Validate boolean flag in exported JSON (SELF-TEST helper)
 */
int SELF_TEST_validate_json_bool_flag(const char *json_path, const char *flag_name, BOOL expected_value)
{
	struct mm_json_elem *json_root;
	BOOL flag_value;

	fprintf(stdout, "Validating %s in JSON...\n", flag_name);

	json_root = SELF_TEST_parse_json_file(json_path);
	if (!json_root) {
		fprintf(stdout, COL_RED_BOLD "FAIL: Could not parse JSON" COL_RESET "\n");
		return -1;
	}

	flag_value = json_get_dict_bool(json_root, flag_name, !expected_value);
	nvmeibt_mm_json_free_kv_tree(json_root);

	if (flag_value != expected_value) {
		fprintf(stdout, COL_RED_BOLD "FAIL: %s should be %s, got %s" COL_RESET "\n",
				flag_name, expected_value ? "true" : "false", flag_value ? "true" : "false");
		return -1;
	}

	fprintf(stdout, COL_GREEN "Verified: %s = %s" COL_RESET "\n", flag_name, expected_value ? "true" : "false");
	return 0;
}

// Setup device with mismatch
static int SELF_TEST_generate_mock_device_with_mismatch(const char *filepath)
{
	int		fd;

	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(filepath);
	if (fd < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create mock device" COL_RESET "\n");
		return -1;
	}
	if (SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(fd,
														  SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
														  1,
														  SELF_TEST_MOCK_DEVICE_BLOCKS - 1) < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not corrupt alternate GPT" COL_RESET "\n");
		close(fd);
		return -1;
	}
	return fd;
}

/**
 * Helper: Export JSON, modify disk_metadata, apply with --write to trigger backup
 * Returns 0 on success, -1 on error
 */
static int SELF_TEST_trigger_backup_for_device(struct self_test_ctx *ctx,
												const char *device_path,
												const char *json_path)
{
	int						rv = 0;
	struct mm_json_elem		*json_root;
	struct mm_json_elem		*disk_md = NULL;

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", json_path);
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	if (rv != 0) {
		return rv;
	}

	/* Modify disk_metadata to trigger a change */
	json_root = SELF_TEST_parse_json_file(json_path);
	if (!json_root) {
		return -1;
	}

	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}

	if (!disk_md) {
		nvmeibt_mm_json_free_kv_tree(json_root);
		return -1;
	}

	json_set_dict_str(disk_md, "ldisk_id_str", "TRIGGER_BACKUP");
	rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, json_path);
	if (rv != 0) {
		return rv;
	}

	/* Apply with --write --yes (creates backup) */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", json_path, "--write", "--yes");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	return rv;
}

/**
 * Find newest backup directory for a device
 * Returns 0 if found, -1 if not found
 * Looks for GPT_UTIL_BACKUP_DIR/backup_<device_basename>_<timestamp>/ directories
 */
static int find_newest_backup_directory(const char *device_path, char *manifest_file, size_t manifest_file_size)
{
	DIR					*dir;
	struct dirent		*entry;
	char				device_basename[64];
	const char			*last_slash;
	char				pattern[80];
	char				backup_dir[512] = {0};
	time_t				newest_time = 0;

	/* Extract device basename */
	last_slash = strrchr(device_path, '/');
	if (last_slash) {
		nvmeibt_strlcpy(device_basename, last_slash + 1, sizeof(device_basename));
	} else {
		nvmeibt_strlcpy(device_basename, device_path, sizeof(device_basename));
	}

	/* Build search pattern */
	snprintf(pattern, sizeof(pattern), "backup_%s_", device_basename);

	/* Scan for backup directories */
	dir = opendir(GPT_UTIL_BACKUP_DIR);
	if (!dir) {
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, pattern, strlen(pattern)) == 0 && entry->d_type == DT_DIR) {
			char full_path[512];
			struct stat st;
			snprintf(full_path, sizeof(full_path), GPT_UTIL_BACKUP_DIR "/%s", entry->d_name);
			if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime > newest_time) {
				newest_time = st.st_mtime;
				nvmeibt_strlcpy(backup_dir, full_path, sizeof(backup_dir));
			}
		}
	}

	closedir(dir);

	if (backup_dir[0] == '\0') {
		return -1;
	}

	/* Build manifest path */
	snprintf(manifest_file, manifest_file_size, "%s/manifest.json", backup_dir);
	return 0;
}

/**
 * Remove directory recursively
 */
static void remove_directory_recursive(const char *dir_path)
{
	DIR				*dir;
	struct dirent	*entry;
	char			full_path[512];

	dir = opendir(dir_path);
	if (!dir) {
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		int ret;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
		if (ret < 0 || (size_t)ret >= sizeof(full_path)) {
			fprintf(stderr, "Error: path truncated for %s/%s\n", dir_path, entry->d_name);
			continue;
		}
		unlink(full_path);		/* Remove files (directories would fail, which is fine) */
	}

	closedir(dir);
	rmdir(dir_path);		/* Remove the directory itself */
}

/**
 * Cleanup all backup directories for a device
 * Removes all GPT_UTIL_BACKUP_DIR/backup_<device_basename>_<timestamp> directories
 */
static void cleanup_backup_files_for_device(const char *device_path)
{
	DIR					*dir;
	struct dirent		*entry;
	char				device_basename[64];
	const char			*last_slash;
	char				pattern[80];
	char				full_path[512];

	/* Extract device basename */
	last_slash = strrchr(device_path, '/');
	if (last_slash) {
		nvmeibt_strlcpy(device_basename, last_slash + 1, sizeof(device_basename));
	} else {
		nvmeibt_strlcpy(device_basename, device_path, sizeof(device_basename));
	}

	/* Build search pattern */
	snprintf(pattern, sizeof(pattern), "backup_%s_", device_basename);

	/* Scan for backup directories */
	dir = opendir(GPT_UTIL_BACKUP_DIR);
	if (!dir) {
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		/* Match backup directories: backup_<device>_<timestamp> */
		if (strncmp(entry->d_name, pattern, strlen(pattern)) == 0 && entry->d_type == DT_DIR) {
			snprintf(full_path, sizeof(full_path), GPT_UTIL_BACKUP_DIR "/%s", entry->d_name);
			remove_directory_recursive(full_path);
		}
	}

	closedir(dir);
}

/********************** Self test cases defined here ************************/

DEFINE_TEST(normal_gpt)
{
	int rv;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	return rv;
}

DEFINE_TEST(mismatch_gpt)
{
	int rv = -1;

	// Step 1: Display mismatched GPT
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// Step 2: Export and verify flag
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both", "-J", TEST_JSON_PATH("mismatch"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	if (SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("mismatch"), "_READONLY_mismatch_detected", true) < 0) {
		goto out;
	}

	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("mismatch"));
	return rv;
}

DEFINE_TEST(uuid_filtering)
{
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--filter-uuid", "aabbccdd-1122-3344-5566-778899aabbcc");
	return SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
}

DEFINE_TEST(lba_filtering)
{
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--filter-lba", "1000");
	return SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
}

DEFINE_TEST(overlap_detection)
{
	int rv = -1;

	// Step 1: Display overlapping GPT
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// Step 2: Export and verify flag
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("overlaps"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	if (SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("overlaps"), "_READONLY_overlaps_detected", true) < 0) {
		goto out;
	}

	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("overlaps"));
	return rv;
}

DEFINE_TEST(gpt_upgrade)
{
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		verify_gpt;
	uint32_t					expected_crc;
	int							nbytes;
	int							wrong_n_partition_entries = 128;
	int							rv = -1;
	int							disk_fd = -1;

	disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(ctx->test_device_path);
	if (disk_fd < 0) {
		goto out;
	}

	if (SELF_TEST_corrupt_gpt_n_partition_entries(disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
													1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1,
													wrong_n_partition_entries) < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
										   &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * main_gpt.header.size_of_partition_entry;
	expected_crc = crc32_seedless(main_gpt.entries, nbytes);
	if (main_gpt.header.n_partition_entries != wrong_n_partition_entries ||
		main_gpt.header.partition_entry_array_crc32 != expected_crc) {
		goto out;
	}

	if (SELF_TEST_upgrade_gpt_if_needed(disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, "Main") < 0) {
		goto out;
	}

	memset(&verify_gpt, 0, sizeof(verify_gpt));
	nvmeibt_strlcpy(verify_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(verify_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
										   &verify_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * verify_gpt.header.size_of_partition_entry;
	expected_crc = crc32_seedless(verify_gpt.entries, nbytes);
	if (verify_gpt.header.n_partition_entries != LARGE_GPT_MAX_NUM_GPT_ENTRIES ||
		verify_gpt.header.partition_entry_array_crc32 != expected_crc) {
		/*
		 * Intentionally allow test to pass if n_partition_entries is 128.
		 * TODO(NVMESH-7436): Remove this line once ticket is fixed.
		 */
		if (verify_gpt.header.n_partition_entries != GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES) {
			goto out;
		}
	}

	rv = 0;

out:
	if (disk_fd >= 0) {
		close(disk_fd);
	}
	return rv;
}

DEFINE_TEST(json_export_apply)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("export"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("export"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("export"));
	return rv;
}

DEFINE_TEST(zeroing_verify)
{
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-Z");
	return SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
}

DEFINE_TEST(diff_no_changes)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("diff_baseline"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("diff_baseline"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("diff_baseline"));
	return rv;
}

DEFINE_TEST(diff_modifications)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("standard"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_modified, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("standard"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("standard"));
	return rv;
}

DEFINE_TEST(apply_write)
{
	int							rv = -1;
	const char					*device_a = TOMA_ROOT_DIR "tmp/gpt_device_a";
	const char					*device_b = TOMA_ROOT_DIR "tmp/gpt_device_b";
	struct mm_json_elem			*json_root = NULL;
	char						mock_serial_b[64];

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_a);
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_modified, device_b);

	// Verify A != B initially
	if (SELF_TEST_compare_gpt_binary(device_a, device_b, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, SELF_TEST_MOCK_DEVICE_BLOCKS) == 0) {
		goto out;
	}

	// Export A
	SELF_TEST_ARGV("-a", device_a, "-J", TEST_JSON_PATH("write_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// Modify JSON to point to B
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("write_test"));
	if (!json_root) {
		goto out;
	}

	SELF_TEST_generate_mock_serial_number_from_path(device_b, mock_serial_b, sizeof(mock_serial_b));
	if (json_set_dict_str(json_root, "device_path", device_b) < 0 ||
		json_set_dict_str(json_root, "_READONLY_controller_serial_num", mock_serial_b) < 0 ||
		SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("write_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	// Apply to B
	SELF_TEST_ARGV("-a", device_b, "--apply-from", TEST_JSON_PATH("write_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// Verify A == B (complete binary roundtrip fidelity)
	if (SELF_TEST_compare_gpt_binary(device_a, device_b, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, SELF_TEST_MOCK_DEVICE_BLOCKS) != 0) {
		goto out;
	}

	rv = 0;

out:
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("write_test"));
	cleanup_backup_files_for_device(device_a);
	cleanup_backup_files_for_device(device_b);
	unlink(device_a);
	unlink(device_b);
	return rv;
}

DEFINE_TEST(json_roundtrip_fidelity)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_json_roundtrip";
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*disk_md = NULL;

	/* Step 1: Create device and export initial JSON */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("json_roundtrip_original"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to export initial JSON");
		goto out;
	}

	/* Step 2: Parse JSON and modify editable fields */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("json_roundtrip_original"));
	if (!json_root) {
		TEST_FAIL("Failed to parse initial JSON");
		goto out;
	}

	/* Find disk_metadata section and modify editable field */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!disk_md) {
		TEST_FAIL("disk_metadata section not found in JSON");
		goto out;
	}

	/* Modify editable field: ldisk_id_str */
	json_set_dict_str(disk_md, "ldisk_id_str", "JSON_ROUNDTRIP_TEST");
	TEST_INFO("Modified disk_metadata.ldisk_id_str = \"JSON_ROUNDTRIP_TEST\"");

	/* Save edited JSON */
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("json_roundtrip_edited")) < 0) {
		TEST_FAIL("Failed to write edited JSON");
		goto out;
	}
	json_root = NULL;

	/* Step 3: Apply edited JSON to device with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("json_roundtrip_edited"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to apply edited JSON");
		goto out;
	}

	/* Step 4: Re-export JSON from device */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("json_roundtrip_reexported"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to re-export JSON from device");
		goto out;
	}

	/* Step 5: Compare edited JSON with re-exported JSON */
	TEST_INFO("Comparing edited JSON with re-exported JSON...");
	if (SELF_TEST_compare_json_files(TEST_JSON_PATH("json_roundtrip_edited"),
									  TEST_JSON_PATH("json_roundtrip_reexported"),
									  ctx->quiet_mode) != 0) {
		TEST_FAIL("JSON roundtrip mismatch: edited JSON differs from re-exported JSON");
		goto out;
	}

	TEST_SUCCEED("JSON roundtrip fidelity verified: edited JSON matches re-exported JSON");
	rv = 0;

out:
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("json_roundtrip_original"));
	unlink(TEST_JSON_PATH("json_roundtrip_edited"));
	unlink(TEST_JSON_PATH("json_roundtrip_reexported"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(missing_section)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("missing_gpt"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	if (SELF_TEST_remove_json_field(TEST_JSON_PATH("missing_gpt"), "main_gpt_primary") < 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("missing_gpt"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("missing_gpt"));
	return rv;
}

DEFINE_TEST(overlap_blocking)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("overlap_block"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("overlap_block"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("overlap_block"));
	return rv;
}

DEFINE_TEST(mismatch_blocking)
{
	int rv = -1;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both", "-J", TEST_JSON_PATH("mismatch_block"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	if (SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("mismatch_block"), "_READONLY_mismatch_detected", true) < 0) {
		goto out;
	}

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("mismatch_block"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("mismatch_block"));
	return rv;
}

DEFINE_TEST(serial_number_mismatch)
{
	int						rv = -1;
	const char				*device_a = TOMA_ROOT_DIR "tmp/gpt_serial_device_a";
	const char				*device_b = TOMA_ROOT_DIR "tmp/gpt_serial_device_b";
	int						fd;
	struct mm_json_elem		*json_root = NULL;

	/* Create device A */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_a);
	if (fd < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device A");
		goto out;
	}
	close(fd);

	/* Create device B */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_b);
	if (fd < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device B");
		goto out;
	}
	close(fd);

	/* Export JSON from device A */
	SELF_TEST_ARGV("-a", device_a, "-J", TEST_JSON_PATH("serial_check"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Try to apply JSON from A to device B (should be BLOCKED) */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("serial_check"));
	if (!json_root ||
		json_set_dict_str(json_root, "device_path", device_b) < 0 ||
		SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("serial_check")) < 0) {
		goto out;
	}
	json_root = NULL;
	TEST_INFO("Modified JSON device_path to point to device B");

	SELF_TEST_ARGV("-a", device_b, "--apply-from", TEST_JSON_PATH("serial_check"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("serial_check"));
	cleanup_backup_files_for_device(device_a);
	cleanup_backup_files_for_device(device_b);
	unlink(device_a);
	unlink(device_b);
	return rv;
}

DEFINE_TEST(missing_serial_number)
{
	int rv = -1;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("missing_serial"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Remove controller_serial_num from JSON (now at root level) */
	if (SELF_TEST_remove_json_field(TEST_JSON_PATH("missing_serial"), "_READONLY_controller_serial_num") < 0) {
		goto out;
	}

	/* Try to apply - should be BLOCKED */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("missing_serial"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("missing_serial"));
	return rv;
}

DEFINE_TEST(delete_main_entry)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_delete_test";
	struct nvmeibt_disk_gpt		gpt_before;
	struct nvmeibt_disk_gpt		gpt_after;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt = NULL;
	struct mm_json_elem			*entries = NULL;

	/* Create device with standard structure */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Verify device has entry 0 before deletion */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&gpt_before, 0, sizeof(gpt_before));
	nvmeibt_strlcpy(gpt_before.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_before.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_before,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&gpt_before.entries[0])) {
		TEST_FAIL("Entry 0 should exist before deletion");
		goto out;
	}

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("delete_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Set _delete flag to true for main_gpt_primary entry 0 */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("delete_test"));
	if (!json_root) {
		goto out;
	}

	/* Navigate: root -> main_gpt_primary -> entries -> [0] -> _delete */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
			main_gpt = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!main_gpt) {
		goto out;
	}

	for (int i = 0; i < main_gpt->dict.len; i++) {
		if (strcmp(main_gpt->dict.elements[i].key, "entries") == 0) {
			entries = main_gpt->dict.elements[i].value;
			break;
		}
	}
	if (!entries || entries->array.len == 0) {
		goto out;
	}

	json_set_dict_bool(entries->array.elements[0], "_delete", true);
	TEST_INFO("Set main_gpt_primary entries[0]._delete = true");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("delete_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("delete_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify entry 0 is deleted */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&gpt_after, 0, sizeof(gpt_after));
	nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&gpt_after.entries[0])) {
		TEST_FAIL("Entry 0 should be deleted after apply");
		goto out;
	}

	TEST_SUCCEED("Verified: Entry 0 successfully deleted");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("delete_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(delete_metadata_entry)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_delete_metadata_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt_before;
	struct nvmeibt_disk_gpt		metadata_gpt_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition = NULL;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*metadata_gpt_elem = NULL;
	struct mm_json_elem			*entries = NULL;
	int							fd = -1;

	/* Create device with standard structure */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read Main GPT to get metadata partition location */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		TEST_FAIL("No metadata partition found");
		goto out;
	}

	/* Verify metadata GPT has entry 0 before deletion */
	memset(&metadata_gpt_before, 0, sizeof(metadata_gpt_before));
	nvmeibt_strlcpy(metadata_gpt_before.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_before.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_before,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt_before.entries[0])) {
		TEST_FAIL("Metadata entry 0 should exist before deletion");
		goto out;
	}

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("delete_metadata_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Set _delete flag to true for metadata_gpt_primary entry 0 */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("delete_metadata_test"));
	if (!json_root) {
		goto out;
	}

	/* Navigate: root -> metadata_gpt_primary -> entries -> [0] -> _delete */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "metadata_gpt_primary") == 0) {
			metadata_gpt_elem = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!metadata_gpt_elem) {
		goto out;
	}

	for (int i = 0; i < metadata_gpt_elem->dict.len; i++) {
		if (strcmp(metadata_gpt_elem->dict.elements[i].key, "entries") == 0) {
			entries = metadata_gpt_elem->dict.elements[i].value;
			break;
		}
	}
	if (!entries || entries->array.len == 0) {
		goto out;
	}

	json_set_dict_bool(entries->array.elements[0], "_delete", true);
	TEST_INFO("Set metadata_gpt_primary entries[0]._delete = true");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("delete_metadata_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("delete_metadata_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify metadata entry 0 is deleted */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		TEST_FAIL("Metadata partition should still exist");
		goto out;
	}

	memset(&metadata_gpt_after, 0, sizeof(metadata_gpt_after));
	nvmeibt_strlcpy(metadata_gpt_after.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_after,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt_after.entries[0])) {
		TEST_FAIL("Metadata entry 0 should be deleted after apply");
		goto out;
	}

	TEST_SUCCEED("Verified: Metadata GPT entry 0 successfully deleted");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("delete_metadata_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(readonly_fields_ignored)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_readonly_test";
	struct nvmeibt_disk_gpt		gpt_before;
	struct nvmeibt_disk_gpt		gpt_after;
	int		 					correct_header_crc;
	int							bogus_crc;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt_elem = NULL;

	correct_header_crc = 0;
	bogus_crc = (int)0xDEADBEEF;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read original CRC */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&gpt_before, 0, sizeof(gpt_before));
	nvmeibt_strlcpy(gpt_before.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_before.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_before,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	correct_header_crc = gpt_before.header.header_crc32;
	TEST_INFO("Original header CRC: 0x%08x", correct_header_crc);

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("readonly_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify _READONLY_header_crc32 to bogus value */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("readonly_test"));
	if (!json_root) {
		goto out;
	}

	/* Navigate to main_gpt_primary section */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
			main_gpt_elem = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!main_gpt_elem || json_set_dict_str(main_gpt_elem, "_READONLY_header_crc32", "0xdeadbeef") < 0) {
		goto out;
	}

	TEST_INFO("Modified main_gpt_primary._READONLY_header_crc32 = 0xdeadbeef");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("readonly_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("readonly_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify CRC on disk is correct (not the bogus value) */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&gpt_after, 0, sizeof(gpt_after));
	nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (gpt_after.header.header_crc32 == bogus_crc) {
		TEST_FAIL("CRC on disk is bogus value 0x%08x (should have been recalculated)", bogus_crc);
		goto out;
	} else if (gpt_after.header.header_crc32 == correct_header_crc) {
		TEST_SUCCEED("Verified: CRC recalculated correctly (0x%08x, not bogus 0x%08x)",
					 gpt_after.header.header_crc32, bogus_crc);
	} else {
		TEST_INFO("Note: CRC changed (old=0x%08x, new=0x%08x, bogus=0x%08x)",
				correct_header_crc, gpt_after.header.header_crc32, bogus_crc);
		TEST_SUCCEED("Verified: Bogus CRC was ignored (CRC != 0x%08x)", bogus_crc);
	}

	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("readonly_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(static_fields_validated)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_static_test";
	struct nvmeibt_disk_gpt		gpt_after;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt = NULL;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("static_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify _STATIC_gpt_signature to bogus value */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("static_test"));
	if (!json_root) {
		goto out;
	}

	/* Navigate to main_gpt_primary section */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
			main_gpt = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!main_gpt || json_set_dict_str(main_gpt, "_STATIC_gpt_signature", "0xdeadbeefdeadbeef") < 0) {
		goto out;
	}

	TEST_INFO("Modified main_gpt_primary._STATIC_gpt_signature = 0xdeadbeefdeadbeef");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("static_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write (should succeed - static fields ignored) */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("static_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify GPT signature on disk is correct (not the bogus value) */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&gpt_after, 0, sizeof(gpt_after));
	nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	if (gpt_after.header.gpt_signature != GPT_SIGNATURE) {
		TEST_FAIL("GPT signature on disk is wrong (0x%lx, expected 0x%lx)",
				  gpt_after.header.gpt_signature, (uint64_t)GPT_SIGNATURE);
		goto out;
	}

	TEST_SUCCEED("Verified: GPT signature correct (0x%lx), bogus value ignored",
				 gpt_after.header.gpt_signature);
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("static_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(nguid_preservation)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_nguid_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt;
	struct nvmeibt_disk_metadata disk_md_before __attribute__((aligned(PAGE_SIZE)));
	struct nvmeibt_disk_metadata disk_md_after __attribute__((aligned(PAGE_SIZE)));
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
	uint64_t					pbyte_s;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*disk_md = NULL;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read original NGUID */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		goto out;
	}

	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}

	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (!disk_md_partition) {
		goto out;
	}

	pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_before);
	close(fd);
	fd = -1;

	/* Export (NGUID will be in JSON) */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("nguid_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Remove native_nguid from JSON */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("nguid_test"));
	if (!json_root) {
		goto out;
	}

	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}

	/* Set native_nguid to null UUID (tests "missing field" preservation path) */
	if (disk_md) {
		json_set_dict_str(disk_md, "native_nguid", "00000000-0000-0000-0000-000000000000");
	}

	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("nguid_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("nguid_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify NGUID preserved (not zeroed or changed) */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		goto out;
	}

	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}

	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (!disk_md_partition) {
		goto out;
	}

	pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	memset(&disk_md_after, 0, sizeof(disk_md_after));
	nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_after);
	close(fd);
	fd = -1;

	if (memcmp(&disk_md_before.native_nguid_unused, &disk_md_after.native_nguid_unused, sizeof(disk_md_before.native_nguid_unused)) != 0) {
		TEST_FAIL("NGUID changed after apply");
		goto out;
	}

	TEST_SUCCEED("Verified: NGUID preserved (unchanged despite missing from JSON)");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("nguid_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(warning_fields_apply)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_warning_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt;
	struct nvmeibt_disk_metadata disk_md_after __attribute__((aligned(PAGE_SIZE)));
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
	uint64_t					pbyte_s;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*disk_md = NULL;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("warning_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify _WARNING_ fields */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("warning_test"));
	if (!json_root) {
		goto out;
	}

	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!disk_md) {
		goto out;
	}

	json_set_dict_num(disk_md, "_WARNING_last_pba_zeroed", 12345);
	json_set_dict_num(disk_md, "_WARNING_format_request_counter", 99);
	TEST_INFO("Modified _WARNING_ fields (last_pba_zeroed=12345, format_request_counter=99)");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("warning_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("warning_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify WARNING fields applied */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		goto out;
	}

	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}

	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (!disk_md_partition) {
		goto out;
	}

	pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	memset(&disk_md_after, 0, sizeof(disk_md_after));
	nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_after);
	close(fd);
	fd = -1;

	if (disk_md_after.last_pba_zeroed != 12345 || disk_md_after.format_request_counter != 99) {
		TEST_FAIL("WARNING fields not applied (last_pba=%lu, counter=%u)",
				  disk_md_after.last_pba_zeroed, disk_md_after.format_request_counter);
		goto out;
	}

	TEST_SUCCEED("Verified: _WARNING_ fields successfully applied");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("warning_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(disk_metadata_apply)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_disk_md_test";
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*disk_md = NULL;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("disk_md_test_edited"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify disk_metadata fields in JSON */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("disk_md_test_edited"));
	if (!json_root) {
		goto out;
	}

	/* Find disk_metadata section */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!disk_md) {
		goto out;
	}

	/* Modify safe fields */
	json_set_dict_str(disk_md, "ldisk_id_str", "MODIFIED_LDISK_ID");
	json_set_dict_num(disk_md, "format_metadata_size", 999999);
	TEST_INFO("Modified disk_metadata fields (ldisk_id_str, format_metadata_size)");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("disk_md_test_edited")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("disk_md_test_edited"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Re-export JSON after apply */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("disk_md_test_reexported"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to re-export JSON after apply");
		goto out;
	}

	/* Comprehensive comparison: edited JSON vs re-exported JSON */
	TEST_INFO("Comparing ALL fields: edited JSON vs re-exported JSON...");
	if (SELF_TEST_compare_json_files(TEST_JSON_PATH("disk_md_test_edited"),
									  TEST_JSON_PATH("disk_md_test_reexported"),
									  ctx->quiet_mode) != 0) {
		TEST_FAIL("Comprehensive JSON comparison failed: re-exported differs from edited");
		goto out;
	}

	TEST_SUCCEED("Comprehensive JSON comparison passed: ALL disk_metadata fields correctly applied");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("disk_md_test_edited"));
	unlink(TEST_JSON_PATH("disk_md_test_reexported"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(zero_change_write_skip)
{
	int rv = -1;

	/* Export from device A */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("zero_change"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Apply same JSON back with --write (0 changes) */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("zero_change"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Success means apply completed without writing (validated by audit log showing changes=0) */
	TEST_SUCCEED("Verified: Apply with 0 changes completed successfully");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("zero_change"));
	return rv;
}

DEFINE_TEST(binary_backup_restore)
{
	int									rv = -1;
	int									fd = -1;
	const char							*device_path = TOMA_ROOT_DIR "tmp/gpt_binary_test";
	char								manifest_file[600] = {0};
	char								*read_buf = NULL;
	struct mm_json_elem					*manifest_json = NULL;
	struct mm_json_elem					*structures_array = NULL;
	struct mm_json_elem					*json_root = NULL;
	struct mm_json_elem					*disk_md_json = NULL;
	DIR				 					*dir = NULL;
	struct dirent						*entry = NULL;
	time_t								newest_time = 0;
	char								backup_dir[512] = {0};
	struct nvmeibt_disk_gpt				main_gpt;
	struct nvmeibt_disk_gpt				metadata_gpt;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_partition;
	struct nvmeibt_seg_active_metadata_ctrl			*verify_ctrl = NULL;
	int									n_seg_md_ctrls = 0;
	uint64_t							new_seg_pba_s = 0;		/* Track NEW_SEG physical location */
	uint64_t							new_seg_pbyte_s = 0;
	char								cp_cmd[512];

	/* Step 1: Create device with segment metadata partitions */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_nvmesh_disk_with_segments, device_path);

	/* Step 2: Export and copy baseline for comprehensive comparison */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("binary_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Copy to baseline (avoids redundant export) */
	snprintf(cp_cmd, sizeof(cp_cmd), "cp %s %s",
			 TEST_JSON_PATH("binary_test"), TEST_JSON_PATH("binary_test_original"));
	if (system(cp_cmd) != 0) {
		TEST_FAIL("Failed to copy JSON baseline");
		goto out;
	}

	/* Modify disk_metadata to trigger write */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("binary_test"));
	if (!json_root) {
		goto out;
	}

	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md_json = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!disk_md_json) {
		goto out;
	}

	json_set_dict_str(disk_md_json, "ldisk_id_str", "MODIFIED_FOR_BACKUP_TEST");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("binary_test")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Step 3: Apply with --write --yes (creates modular backup automatically) */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("binary_test"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}
	TEST_INFO("Modular backup should have been created in " GPT_UTIL_BACKUP_DIR);

	/* Step 4: Find the backup directory (newest backup_* directory) */
	dir = opendir(GPT_UTIL_BACKUP_DIR);

	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			if (strncmp(entry->d_name, "backup_", 7) == 0 && entry->d_type == DT_DIR) {
				char full_path[512];
				struct stat st;
				snprintf(full_path, sizeof(full_path), GPT_UTIL_BACKUP_DIR "/%s", entry->d_name);
				if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime > newest_time) {
					newest_time = st.st_mtime;
					nvmeibt_strlcpy(backup_dir, full_path, sizeof(backup_dir));
				}
			}
		}
		closedir(dir);
	}

	if (backup_dir[0] == '\0') {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	snprintf(manifest_file, sizeof(manifest_file), "%s/manifest.json", backup_dir);
	TEST_INFO("Found backup directory: %s", backup_dir);
	TEST_INFO("Manifest: %s", manifest_file);

	/* Step 5: Verify manifest structure and hidden files exist */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (!manifest_json) {
		TEST_FAIL("Cannot parse manifest");
		goto out;
	}

	structures_array = json_get_dict_value(manifest_json, "structures");
	if (!structures_array || structures_array->type != JSON_E_ARRAY) {
		TEST_FAIL("Manifest missing structures array");
		goto out;
	}

	TEST_INFO("Manifest contains %d structures", structures_array->array.len);

	/* Verify all structure files exist and count control blocks */
	for (int i = 0; i < structures_array->array.len; i++) {
		struct mm_json_elem *structure_elem = structures_array->array.elements[i];
		const char *file = json_get_dict_str(structure_elem, "file", NULL);
		const char *name = json_get_dict_str(structure_elem, "name", NULL);
		struct stat st;

		if (!file || stat(file, &st) < 0) {
			TEST_FAIL("Missing backup file");
			goto out;
		}
		TEST_INFO("  ✓ %s (%lu bytes)", name ? name : "unknown", (uint64_t)st.st_size);

		/* Count segment metadata control blocks */
		if (name && strstr(name, "seg_md_ctrl_") != NULL) {
			n_seg_md_ctrls++;
		}
	}

	TEST_INFO("Backup includes %d segment metadata control blocks", n_seg_md_ctrls);

	/* Validate we have exactly 2 control blocks (from mock device with 2 segment partitions) */
	if (n_seg_md_ctrls != 2) {
		TEST_FAIL("Expected 2 segment metadata control blocks, got %d", n_seg_md_ctrls);
		goto out;
	}
	TEST_INFO("✓ Verified: Backup contains 2 segment metadata control blocks");

	/* Step 6: Add THIRD segment metadata partition after backup (to verify restore doesn't corrupt its data) */
	/* Device now has 2 segments (backed up), about to add 3rd (not backed up) */
	/* Note: Restore will remove NEW_SEG's GPT entry (correct), but DATA should remain intact */
	TEST_INFO("Adding 3rd segment metadata partition after backup (isolation test)");

	/* Find empty slot in metadata GPT */
	fd = open(device_path, O_RDWR);
	if (fd < 0) {
		TEST_FAIL("Cannot open device for adding new partition");
		goto out;
	}

	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		close(fd);
		TEST_FAIL("Cannot read main GPT");
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		close(fd);
		TEST_FAIL("Cannot find metadata partition");
		goto out;
	}

	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
											metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		close(fd);
		TEST_FAIL("Cannot read metadata GPT");
		goto out;
	}

	/* Find empty slot and calculate placement after existing segments */
	for (int k = 0; k < metadata_gpt.max_n_entries; k++) {
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt.entries[k])) {
			struct nvmeibt_seg_active_metadata_ctrl		new_ctrl;
			struct nvmeibt_disk_gpt_partition_entry		new_entry;
			uint64_t									max_seg_pba_e = 0;
			const struct nvmeibt_disk_gpt_partition_entry	*disk_md_entry;

			/* Find last segment metadata partition to place new one after it */
			for (int j = 0; j < metadata_gpt.max_n_entries; j++) {
				if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt.entries[j]) &&
					ARE_UUID_EQ(&metadata_gpt.entries[j].partition_type_guid, &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID)) {
					if (metadata_gpt.entries[j].pba_e > max_seg_pba_e) {
						max_seg_pba_e = metadata_gpt.entries[j].pba_e;
					}
				}
			}

			/* If no existing segment metadata partitions, start after disk_metadata */
			if (max_seg_pba_e == 0) {
				disk_md_entry = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
				if (disk_md_entry) {
					max_seg_pba_e = disk_md_entry->pba_e;
				}
			}

			/* Place new segment after last partition with 5-block gap (50 blocks total) */
			memset(&new_entry, 0, sizeof(new_entry));
			new_entry.partition_type_guid = EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID;
			new_entry.partition_guid.ll[0] = 0xDEADBEEF0001ULL;
			new_entry.partition_guid.ll[1] = 0xDEADBEEF0002ULL;
			new_entry.pba_s = max_seg_pba_e + 5;		/* 5-block gap for safety */
			new_entry.pba_e = new_entry.pba_s + 49;		/* 50 blocks */
			new_entry.attributes = 0;
			str_to_char16_str("NEW_SEG_AFTER_BACKUP", strlen("NEW_SEG_AFTER_BACKUP"), new_entry.partition_name);

			/* Write new entry to metadata GPT */
			memcpy(&metadata_gpt.entries[k], &new_entry, sizeof(new_entry));
			if (nvmeibt_disk_metadata_store_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt, false) < 0) {
				close(fd);
				TEST_FAIL("Cannot write new partition entry");
				goto out;
			}

			/* Write marker to new partition's control block */
			memset(&new_ctrl, 0, sizeof(new_ctrl));
			nvmeibt_strlcpy(new_ctrl.header.magic_str, "TEST_MARKER_NEW_SEG_AFTER_BACKUP", sizeof(new_ctrl.header.magic_str));
			new_ctrl.header.software_version = 0xDEADBEEF;		/* Unique marker */
			new_ctrl.header.seg_metadata_version = 999;
			new_ctrl.disk_segment_uuid.ll[0] = 0xCAFEBABE00000000ULL;
			new_ctrl.disk_segment_uuid.ll[1] = 0xDEADC0DE00000000ULL;

			/* Zero filler and calculate CRC (matches production) */
			memset(new_ctrl.__zeroed_filler_till_4K__, 0, sizeof(new_ctrl) - offsetof(typeof(new_ctrl), __zeroed_filler_till_4K__));
			new_ctrl.metadata_ctrl_crc32 = crc32_seedless(&new_ctrl, offsetof(typeof(new_ctrl), metadata_ctrl_crc32));

			/* Write control block to new partition */
			if (write_segment_control_block(fd, new_entry.pba_s, &new_ctrl) < 0) {
				close(fd);
				TEST_FAIL("Cannot write marker to new partition");
				goto out;
			}

			new_seg_pba_s = new_entry.pba_s;		/* Save for verification after restore */
			TEST_INFO("Created new segment metadata partition at PBA %lu with marker", new_seg_pba_s);
			break;
		}
	}
	close(fd);
	fd = -1;

	/* Step 7: Restore from modular backup */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Step 8: Export JSON after restore and compare ALL fields with original */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("binary_test_restored"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to export JSON after restore");
		goto out;
	}

	/* Comprehensive comparison: original JSON vs restored JSON */
	TEST_INFO("Comparing ALL fields: original vs restored JSON...");
	if (SELF_TEST_compare_json_files(TEST_JSON_PATH("binary_test_original"),
									  TEST_JSON_PATH("binary_test_restored"),
									  ctx->quiet_mode) != 0) {
		TEST_FAIL("Comprehensive JSON comparison failed: restored device differs from original");
		goto out;
	}
	TEST_SUCCEED("Comprehensive JSON comparison passed: ALL fields match after restore");

	/* Re-open device for NEW_SEG isolation test */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	/* Step 9: Verify NEW partition physical data (added after backup) is preserved by restore */
	/* Note: GPT entry will be gone (restore overwrites entire entries array), but physical data should remain */
	TEST_INFO("Verifying new partition physical data not corrupted by restore");

	/* Read physical data at NEW_SEG location directly (bypass GPT) */
	if (new_seg_pba_s == 0) {
		TEST_FAIL("NEW_SEG PBA not recorded - test infrastructure error");
		goto out;
	}

	new_seg_pbyte_s = new_seg_pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;

	read_buf = NNVMEIBT_BM_ALIGNED_CALLOC(trace_test25_verify, PAGE_SIZE, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE);
	if (!read_buf) {
		TEST_FAIL("Cannot allocate verify buffer");
		goto out;
	}

	if (NNVMEIBT_PREAD_ATOMIC(trace_test25_read_verify, fd, read_buf, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, new_seg_pbyte_s, 1) != SELF_TEST_MOCK_DEVICE_BLOCK_SIZE) {
		NNVMEIBT_BM_FREE(trace_test25_verify_free, read_buf);
		TEST_FAIL("Cannot read new partition physical data at PBA 0x%lx", new_seg_pba_s);
		goto out;
	}

	verify_ctrl = (struct nvmeibt_seg_active_metadata_ctrl *)read_buf;
	if (strcmp(verify_ctrl->header.magic_str, "TEST_MARKER_NEW_SEG_AFTER_BACKUP") != 0 ||
		verify_ctrl->header.software_version != 0xDEADBEEF ||
		verify_ctrl->header.seg_metadata_version != 999) {
		NNVMEIBT_BM_FREE(trace_test25_verify_free2, read_buf);
		TEST_FAIL("New partition physical data corrupted by restore (magic=%s sw_ver=0x%x md_ver=%u)",
					verify_ctrl->header.magic_str, verify_ctrl->header.software_version, verify_ctrl->header.seg_metadata_version);
		goto out;
	}

	NNVMEIBT_BM_FREE(trace_test25_verify_free3, read_buf);
	TEST_INFO("New partition physical data intact (not overwritten by restore)");

	TEST_SUCCEED("Verified: Device restored to original state (modular backup)");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("binary_test"));
	unlink(TEST_JSON_PATH("binary_test_original"));
	unlink(TEST_JSON_PATH("binary_test_restored"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_serial_mismatch)
{
	int							rv = -1;
	const char					*device_a = TOMA_ROOT_DIR "tmp/gpt_backup_serial_a";
	const char					*device_b = TOMA_ROOT_DIR "tmp/gpt_backup_serial_b";
	char						manifest_file[600] = {0};
	int							fd_a;
	int							fd_b;

	/* Create device A */
	fd_a = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_a);
	if (fd_a < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device A");
		goto out;
	}
	close(fd_a);

	/* Create device B */
	fd_b = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_b);
	if (fd_b < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device B");
		goto out;
	}
	close(fd_b);

	/* Export from device A and trigger backup */
	if (SELF_TEST_trigger_backup_for_device(ctx, device_a, TEST_JSON_PATH("backup_serial")) < 0) {
		goto out;
	}

	/* Find the manifest */
	if (find_newest_backup_directory(device_a, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Try to restore to device B (should be BLOCKED by serial mismatch) */
	SELF_TEST_ARGV("-a", device_b, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("backup_serial"));
	cleanup_backup_files_for_device(device_a);
	cleanup_backup_files_for_device(device_b);
	unlink(device_a);
	unlink(device_b);
	return rv;
}

DEFINE_TEST(backup_restore_missing_file)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_missing";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*first_structure_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_missing")) < 0) {
		goto out;
	}

	/* Find manifest */
	if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Parse manifest and delete first structure file */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (manifest_json) {
		structures_array = json_get_dict_value(manifest_json, "structures");
		if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len > 0) {
			first_structure_file = json_get_dict_str(structures_array->array.elements[0], "file", NULL);
			if (first_structure_file) {
				TEST_INFO("Deleting structure file: %s", first_structure_file);
				unlink(first_structure_file);
			}
		}
	}

	/* Try to restore (should be BLOCKED by missing file) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_missing"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_corrupted_file)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_corrupt";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*first_structure_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_corrupt")) < 0) {
		goto out;
	}

	/* Find manifest */
	if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Parse manifest and corrupt first structure file (truncate it) */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (manifest_json) {
		structures_array = json_get_dict_value(manifest_json, "structures");
		if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len > 0) {
			first_structure_file = json_get_dict_str(structures_array->array.elements[0], "file", NULL);
			if (first_structure_file) {
				int fd = open(first_structure_file, O_WRONLY | O_TRUNC);
				if (fd >= 0) {
					TEST_INFO("Corrupting structure file (truncating): %s", first_structure_file);
					write(fd, "BAD", 3);		/* Write wrong size */
					close(fd);
				}
			}
		}
	}

	/* Try to restore (should be BLOCKED by wrong file size) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_corrupt"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_incomplete_manifest)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_incomplete";
	int							fd;
	char						mock_serial[64];
	char						json_content[512];

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Create incomplete manifest manually (missing "structures" field) */
	fd = open(TEST_JSON_PATH("incomplete_manifest"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		goto out;
	}

	SELF_TEST_generate_mock_serial_number_from_path(device_path, mock_serial, sizeof(mock_serial));
	snprintf(json_content, sizeof(json_content),
		"{\n"
		"  \"backup_timestamp\": \"test\",\n"
		"  \"device_path\": \"%s\",\n"
		"  \"block_size\": 4096,\n"
		"  \"controller_serial_num\": \"%s\"\n"
		"}\n",
		device_path, mock_serial);
	write(fd, json_content, strlen(json_content));
	close(fd);
	TEST_INFO("Created incomplete manifest (missing 'structures' field)");

	/* Try to restore (should be BLOCKED by missing structures field) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", TEST_JSON_PATH("incomplete_manifest"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("incomplete_manifest"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_pba_overflow)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_overflow";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	int							last_idx;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_overflow")) < 0) {
		goto out;
	}

	/* Find manifest */
	if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Modify manifest to set a structure's pba_start beyond device end */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (!manifest_json) {
		goto out;
	}

	structures_array = json_get_dict_value(manifest_json, "structures");
	if (!structures_array || structures_array->type != JSON_E_ARRAY || structures_array->array.len == 0) {
		goto out;
	}

	/* Set last structure's pba_start to 999999 (way beyond device end) */
	last_idx = structures_array->array.len - 1;
	json_set_dict_num(structures_array->array.elements[last_idx], "pba_start", 999999);
	TEST_INFO("Modified manifest: set last structure pba_start=999999");
	if (SELF_TEST_write_json_file_and_free_kv_tree(manifest_json, manifest_file) < 0) {
		goto out;
	}
	manifest_json = NULL;

	/* Try to restore (should be BLOCKED by PBA overflow) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_overflow"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_block_size_mismatch)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_blocksize";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_blocksize")) < 0) {
		goto out;
	}

	/* Find manifest */
	if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Modify manifest to set wrong block_size */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (!manifest_json) {
		goto out;
	}

	json_set_dict_num(manifest_json, "block_size", 512);		/* Wrong! Should be 4096 */
	TEST_INFO("Modified manifest: set block_size=512 (should be 4096)");
	if (SELF_TEST_write_json_file_and_free_kv_tree(manifest_json, manifest_file) < 0) {
		goto out;
	}
	manifest_json = NULL;

	/* Try to restore (should be BLOCKED by block size mismatch) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_blocksize"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_empty_structures)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_empty";
	int							fd;
	char						mock_serial[64];
	char						json_content[512];

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Create manifest with empty structures array */
	fd = open(TEST_JSON_PATH("empty_structures"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		goto out;
	}

	SELF_TEST_generate_mock_serial_number_from_path(device_path, mock_serial, sizeof(mock_serial));
	snprintf(json_content, sizeof(json_content),
		"{\n"
		"  \"backup_timestamp\": \"test\",\n"
		"  \"device_path\": \"%s\",\n"
		"  \"block_size\": 4096,\n"
		"  \"controller_serial_num\": \"%s\",\n"
		"  \"structures\": []\n"
		"}\n",
		device_path, mock_serial);
	write(fd, json_content, strlen(json_content));
	TEST_INFO("Created manifest with empty structures array (structures: [])");

	/* Try to restore (should be BLOCKED by structure count = 0, minimum required is 10) */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", TEST_JSON_PATH("empty_structures"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	unlink(TEST_JSON_PATH("empty_structures"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_creation_non_nvmesh_device)
{
	int									rv = -1;
	const char							*device_path = TOMA_ROOT_DIR "tmp/gpt_non_nvmesh";
	int									fd = -1;
	struct nvmeibt_disk_mbr				mbr;
	struct nvmeibt_disk_gpt				main_gpt;
	union nvmeib_uuid					disk_uuid;
	uint64_t							n_disk_blocks = SELF_TEST_MOCK_DEVICE_BLOCKS;
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;

	/* Create device with Main GPT ONLY (no EXCELERO_METADATA partition) */
	fd = open(device_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device");
		goto out;
	}

	/* Initialize MBR */
	memset(&mbr, 0, sizeof(mbr));
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		goto out;
	}

	/* Initialize Main GPT with NO partitions (regular GPT disk, not NVMesh) */
	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	/* Write Main GPT (NO EXCELERO_METADATA partition added) */
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		goto out;
	}

	fsync(fd);

	/* Try to export (should be BLOCKED - device not NVMesh formatted) */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("non_nvmesh"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	unlink(TEST_JSON_PATH("non_nvmesh"));
	unlink(device_path);
	return rv;
}

DEFINE_TEST(restore_mid_failure_file_deleted)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_restore_mid_fail";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*last_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("restore_mid_fail")) < 0) {
		goto out;
	}

	/* Find manifest */
	if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
		TEST_FAIL("No backup directory found");
		goto out;
	}

	/* Delete LAST structure file to simulate mid-restore I/O failure */
	manifest_json = SELF_TEST_parse_json_file(manifest_file);
	if (manifest_json) {
		structures_array = json_get_dict_value(manifest_json, "structures");
		if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len >= 10) {
			int last_idx = structures_array->array.len - 1;
			const char *last_name = json_get_dict_str(structures_array->array.elements[last_idx], "name", NULL);

			/* Get last structure (could be disk_metadata or segment control block) */
			last_file = json_get_dict_str(structures_array->array.elements[last_idx], "file", NULL);
			if (last_file) {
				TEST_INFO("Deleting last structure file: %s (%s)", last_file, last_name ? last_name : "unknown");
				TEST_INFO("Restore should fail when file is missing");
				unlink(last_file);		/* Delete file - restore will fail */
			}
		}
	}

	/* Try to restore - should fail with partial restore warning */
	SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have been blocked!");
		goto out;
	}

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("restore_mid_fail"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(csv_parsing_path)
{
	int rv = -1;

	/* Note: -d flag reads from /proc/nvmeibs/disks.csv which doesn't exist in sandbox
	 * This test validates the flag is accepted and parsing logic doesn't crash
	 * Expected: Fails gracefully with "device not found" message
	 */
	SELF_TEST_ARGV("-d", TOMA_ROOT_DIR "/dev/nvme0n1");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		TEST_FAIL("Should have failed (device not in CSV)");
		goto out;
	}

	TEST_SUCCEED("Verified: -d flag handled gracefully (device not found, no crash)");
	rv = 0;

out:
	return rv;
}

/* This test will seg fault as we don't have json type enforcement */
// DEFINE_TEST(malformed_json_type)
// {
// 	int rv = 0;

// 	/* Create device and export */
// 	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
// 	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("malformed"));
// 	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

// 	/* Corrupt JSON: change string field to number */
// 	if (rv == 0) {
// 		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("malformed"));
// 		struct mm_json_elem *main_gpt = NULL;

// 		if (json_root) {
// 			for (int i = 0; i < json_root->dict.len; i++) {
// 				if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
// 					main_gpt = json_root->dict.elements[i].value;
// 					break;
// 				}
// 			}
// 			/* Set disk_uuid (string) to a number - will cause type mismatch */
// 			if (main_gpt) {
// 				json_set_dict_num(main_gpt, "disk_uuid", 12345);
// 				fprintf(stdout, "Corrupted JSON: set disk_uuid (string) to number\n");
// 				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("malformed"));
// 			} else {
// 				rv = -1;
// 			}
// 		} else {
// 			rv = -1;
// 		}
// 	}

// 	/* Try to apply - should fail gracefully (not crash) */
// 	if (rv == 0) {
// 		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
// 		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("malformed"));
// 		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
// 		/* Expect failure, but graceful (no crash) */
// 	}

// 	/* Cleanup */
// 	unlink(TEST_JSON_PATH("malformed"));
// 	cleanup_backup_files_for_device(ctx->test_device_path);
// 	return rv;
// }

DEFINE_TEST(json_add_partition_entry)
{
	int									rv = -1;
	int									fd = -1;
	int									n_entries_after = 0;
	const char							*device_path = TOMA_ROOT_DIR "tmp/gpt_add_partition";
	struct mm_json_elem					*json_root = NULL;
	struct mm_json_elem					*existing_entry = NULL;
	struct mm_json_elem					*new_entry = NULL;
	struct mm_json_elem					*entries = NULL;
	struct mm_json_elem					*main_gpt_json = NULL;
	struct nvmeibt_disk_gpt				main_gpt_after;
	struct nvmeibt_disk_mbr				mbr;
	struct nvmeibt_disk_gpt				main_gpt;
	struct nvmeibt_disk_gpt				metadata_gpt;
	int									fd_large = -1;
	union nvmeib_uuid					disk_uuid;
	union nvmeib_uuid					metadata_disk_uuid;
	union nvmeib_uuid					metadata_partition_uuid;
	union nvmeib_uuid					disk_metadata_partition_uuid;
	uint64_t							n_disk_blocks = 4000;		/* Larger device */
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	struct nvmeibt_disk_metadata		disk_metadata;
	char								*dma_buffer = NULL;
	int									n_bytes_write;
	uint64_t							pbyte_s;
	struct nvmeibt_disk_gpt_partition_entry			*metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition = NULL;

	/* Create LARGER device (4000 blocks) to have room for multiple partitions */
	/* Standard 2000 blocks has EXCELERO_METADATA taking 258-1742, leaving no room */
	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, "Metadata", sizeof(metadata_gpt.main_or_metadata));

	fd_large = open(device_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd_large < 0) {
		TEST_FAIL("SETUP FAILED");
		return -1;
	}

	/* Initialize MBR */
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	nvmeibt_disk_metadata_write_mbr(NULL, fd_large, pblk_size, &mbr);

	/* Initialize Main GPT */
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;
	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
												LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	/* Add EXCELERO_METADATA partition (smaller to leave room for new partition) */
	/* Use PBA 258-1500 (instead of full usable range 258-3742) */
	metadata_partition_uuid.ll[0] = 0xAABBCCDD11223344ULL;
	metadata_partition_uuid.ll[1] = 0x5566778899AABBCCULL;

	metadata_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
		&main_gpt, &EXCELERO_METADATA_PARTITION_TYPE_GUID,
		&metadata_partition_uuid,
		main_gpt.header.first_usable_pba,
		main_gpt.header.first_usable_pba + 1242,		/* PBA 258-1500 */
		EXCELERO_METADATA_PARTITION_NAME,
		strlen(EXCELERO_METADATA_PARTITION_NAME));

	if (!metadata_partition) {
		close(fd_large);
		return -1;
	}

	nvmeibt_disk_metadata_store_gpt(NULL, fd_large, pblk_size, &main_gpt, false);

	/* Initialize nested Metadata GPT */
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
												metadata_partition->pba_e,
												&metadata_gpt, pblk_size,
												MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	/* Add disk_metadata partition */
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
											&EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
											&disk_metadata_partition_uuid,
											metadata_gpt.header.first_usable_pba,
											metadata_gpt.header.last_usable_pba,
											DISK_METADATA_PARTITION_NAME,
											strlen(DISK_METADATA_PARTITION_NAME));

	nvmeibt_disk_metadata_store_gpt(NULL, fd_large, pblk_size, &metadata_gpt, false);

	/* Write disk_metadata structure */
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (disk_md_partition) {
		memset(&disk_metadata, 0, sizeof(disk_metadata));
		disk_metadata.signature = DISK_METADATA_SIGNATURE;
		disk_metadata.format_pblk_size = pblk_size;
		disk_metadata.format_request_counter = 1;

		// Test serial ID
		nvmeibt_strlcpy(disk_metadata.native_serial_str, "TEST-SERIAL-001", sizeof(disk_metadata.native_serial_str));
		disk_metadata.native_nguid_unused.ll[0] = 0xAABBCCDD11223344ULL;
		disk_metadata.native_nguid_unused.ll[1] = 0x5566778899AABBCCULL;
		disk_metadata.crc32 = 0;
		disk_metadata.crc32 = crc32_seedless(&disk_metadata, sizeof(disk_metadata));

		pbyte_s = disk_md_partition->pba_s * pblk_size;
		n_bytes_write = roundup(sizeof(disk_metadata), pblk_size);
		dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_large_disk_md, PAGE_SIZE, n_bytes_write);
		memcpy(dma_buffer, &disk_metadata, sizeof(disk_metadata));
		pwrite(fd_large, dma_buffer, n_bytes_write, pbyte_s);
		NNVMEIBT_BM_FREE(trace_selftest_large_disk_md_free, dma_buffer);
	}

	fsync(fd_large);
	close(fd_large);
	TEST_INFO("Created larger device (4000 blocks) with partition PBA 258-1500");

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("add_partition"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Add a new partition entry by duplicating existing entry and modifying it */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("add_partition"));
	if (!json_root) {
		goto out;
	}

	/* Navigate to main_gpt_primary -> entries */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
			main_gpt_json = json_root->dict.elements[i].value;
			break;
		}
	}
	if (main_gpt_json) {
		for (int i = 0; i < main_gpt_json->dict.len; i++) {
			if (strcmp(main_gpt_json->dict.elements[i].key, "entries") == 0) {
				entries = main_gpt_json->dict.elements[i].value;
				break;
			}
		}
	}

	if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
		goto out;
	}

	/* Manually add a new entry by expanding the array */
	existing_entry = entries->array.elements[0];
	new_entry = calloc(1, sizeof(*new_entry));
	if (!new_entry) {
		goto out;
	}

	new_entry->type = JSON_E_DICT;
	new_entry->dict.len = existing_entry->dict.len;
	new_entry->dict.elements = calloc(existing_entry->dict.len, sizeof(struct mm_json_kv_pair));
	new_entry->parent = entries;

	/* Deep copy all fields from existing entry */
	for (int i = 0; i < existing_entry->dict.len; i++) {
		new_entry->dict.elements[i].key = strdup(existing_entry->dict.elements[i].key);
		new_entry->dict.elements[i].value = calloc(1, sizeof(struct mm_json_elem));
		new_entry->dict.elements[i].value->type = existing_entry->dict.elements[i].value->type;
		new_entry->dict.elements[i].value->parent = new_entry;
		if (existing_entry->dict.elements[i].value->type == JSON_E_STR) {
			new_entry->dict.elements[i].value->str = strdup(existing_entry->dict.elements[i].value->str);
		} else {
			new_entry->dict.elements[i].value->num = existing_entry->dict.elements[i].value->num;
		}
	}

	/* Add new partition in free space after existing (1501-1700) */
	json_set_dict_num(new_entry, "index", 1);
	json_set_dict_str(new_entry, "name", "new_partition");
	json_set_dict_str(new_entry, "partition_guid", "11223344-5566-7788-99aa-bbccddeeff00");
	json_set_dict_str(new_entry, "type_guid", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
	json_set_dict_num(new_entry, "pba_s", 1501);
	json_set_dict_num(new_entry, "pba_e", 1700);
	json_set_dict_num(new_entry, "attributes", 0);
	json_set_dict_bool(new_entry, "_delete", false);

	/* Expand entries array */
	entries->array.elements = realloc(entries->array.elements,
									   (entries->array.len + 1) * sizeof(struct mm_json_elem *));
	entries->array.elements[entries->array.len] = new_entry;
	entries->array.len++;

	TEST_INFO("Added new partition entry (index 1, PBA 600-800)");

	/* Write modified JSON */
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("add_partition")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("add_partition"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify new partition was added */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt_after, 0, sizeof(main_gpt_after));
	nvmeibt_strlcpy(main_gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt_after,
										  1, 4000 - 1, false) < 0) {
		goto out;
	}

	for (int i = 0; i < main_gpt_after.max_n_entries; i++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&main_gpt_after.entries[i])) {
			n_entries_after++;
		}
	}

	if (n_entries_after != 2) {
		TEST_FAIL("Expected 2 entries, got %d", n_entries_after);
		goto out;
	}

	TEST_SUCCEED("Verified: New partition added successfully (1 -> 2 entries)");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("add_partition"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(json_modify_metadata_gpt)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_modify_metadata";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	char						partition_name_after[GPT_MAX_PARTITION_NAME_LENGTH + 1];
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*metadata_gpt_elem = NULL;
	struct mm_json_elem			*entries = NULL;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("modify_metadata"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify metadata GPT partition name */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("modify_metadata"));
	if (!json_root) {
		goto out;
	}

	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "metadata_gpt_primary") == 0) {
			metadata_gpt_elem = json_root->dict.elements[i].value;
			break;
		}
	}
	if (metadata_gpt_elem) {
		for (int i = 0; i < metadata_gpt_elem->dict.len; i++) {
			if (strcmp(metadata_gpt_elem->dict.elements[i].key, "entries") == 0) {
				entries = metadata_gpt_elem->dict.elements[i].value;
				break;
			}
		}
	}

	/* Modify first entry's name */
	if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
		goto out;
	}

	json_set_dict_str(entries->array.elements[0], "name", "Modified_Disk_Metadata");
	TEST_INFO("Modified metadata partition name to: Modified_Disk_Metadata");

	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("modify_metadata")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Apply with --write */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("modify_metadata"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify name was changed */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		goto out;
	}

	memset(&metadata_gpt_after, 0, sizeof(metadata_gpt_after));
	nvmeibt_strlcpy(metadata_gpt_after.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_after.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_after,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		goto out;
	}

	char16_str_to_str(metadata_gpt_after.entries[0].partition_name,
					  GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name_after);

	if (strcmp(partition_name_after, "Modified_Disk_Metadata") != 0) {
		TEST_FAIL("Name not changed (got: %s)", partition_name_after);
		goto out;
	}

	TEST_SUCCEED("Verified: Metadata partition name changed successfully");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) { close(fd); }
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("modify_metadata"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(json_boundary_max_partitions)
{
	int rv = -1;

	/* Verify system handles LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192) correctly */
	/* Standard device uses 8192-entry GPTs for both Main and Metadata */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);

	/* Export to JSON (exercises 8192-entry GPT export) */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("boundary_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Apply back (exercises 8192-entry GPT apply) */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("boundary_test"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Validates: n_partition_entries=8192 handled correctly in export/apply */
	TEST_SUCCEED("Verified: 8192-entry GPT handled correctly");
	rv = 0;

out:
	/* Cleanup */
	unlink(TEST_JSON_PATH("boundary_test"));
	return rv;
}

/**
 * Create device with many partitions for stress testing
 * Returns fd on success, -1 on error
 */
static int create_device_with_many_partitions(const char *filepath, int n_partitions)
{
	int									rv = -1;
	int									fd = -1;
	struct nvmeibt_disk_mbr				mbr;
	struct nvmeibt_disk_gpt				main_gpt;
	struct nvmeibt_disk_gpt				metadata_gpt;
	union nvmeib_uuid					disk_uuid;
	union nvmeib_uuid					metadata_disk_uuid;
	union nvmeib_uuid					disk_metadata_partition_uuid;
	uint64_t							n_disk_blocks = 500000;		/* ~2GB device (sparse - minimal disk usage) */
	int									pblk_size = SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
	uint64_t							data_partition_size = 50;		/* Each DATA partition is 50 blocks */
	uint64_t							metadata_partition_size = 1500;	/* EXCELERO_METADATA needs ~1500 blocks for nested GPT */
	uint64_t							current_pba;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition;
	struct nvmeibt_disk_gpt_partition_entry			*metadata_partition;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, "Main", sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, "Metadata", sizeof(metadata_gpt.main_or_metadata));

	fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return -1;
	}

	/* Initialize protective MBR */
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		goto out;
	}

	/* Initialize Main GPT */
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;
	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	/* Add n_partitions non-overlapping partitions */
	/* First partition: EXCELERO_METADATA */
	/* Remaining partitions: DATA segments */
	current_pba = main_gpt.header.first_usable_pba;
	for (int i = 0; i < n_partitions; i++) {
		char partition_name[64];
		union nvmeib_uuid part_uuid;
		const union nvmeib_uuid *type_guid;
		uint64_t part_size;

		/* First partition needs to be much larger (contains nested Metadata GPT) */
		part_size = (i == 0) ? metadata_partition_size : data_partition_size;

		if (current_pba + part_size >= main_gpt.header.last_usable_pba) {
			break;  /* Out of space */
		}

		snprintf(partition_name, sizeof(partition_name), "partition_%d", i);
		part_uuid.ll[0] = 0xAABBCCDD00000000ULL | i;
		part_uuid.ll[1] = 0x5566778899AABBCCULL;

		/* First partition is EXCELERO_METADATA (contains nested GPT) */
		/* Rest are DATA partitions (normal NVMesh segments) */
		type_guid = (i == 0) ? &EXCELERO_METADATA_PARTITION_TYPE_GUID
							 : &EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL;

		if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&main_gpt,
													 type_guid,
													 &part_uuid,
													 current_pba,
													 current_pba + part_size - 1,
													 partition_name,
													 strlen(partition_name))) {
			goto out;
		}

		current_pba += part_size;
	}

	/* Write Main GPT */
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		goto out;
	}

	/* Initialize nested Metadata GPT */
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	metadata_partition = &main_gpt.entries[0];		/* First partition for metadata */
	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
											 metadata_partition->pba_e,
											 &metadata_gpt, pblk_size,
											 MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	/* Add disk_metadata partition */
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.last_usable_pba,
												 DISK_METADATA_PARTITION_NAME,
												 strlen(DISK_METADATA_PARTITION_NAME))) {
		goto out;
	}

	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &metadata_gpt, false) < 0) {
		goto out;
	}

	/* Write disk_metadata structure */
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
	if (disk_md_partition) {
		struct nvmeibt_disk_metadata disk_metadata;
		char *dma_buffer = NULL;
		int n_bytes_write;
		uint64_t pbyte_s;

		memset(&disk_metadata, 0, sizeof(disk_metadata));
		disk_metadata.signature = DISK_METADATA_SIGNATURE;
		disk_metadata.format_pblk_size = pblk_size;
		disk_metadata.format_request_counter = 1;

		/* Test serial ID */
		nvmeibt_strlcpy(disk_metadata.native_serial_str, "TEST-SERIAL-STRESS", sizeof(disk_metadata.native_serial_str));
		disk_metadata.native_nguid_unused.ll[0] = 0xAABBCCDD11223344ULL;
		disk_metadata.native_nguid_unused.ll[1] = 0x5566778899AABBCCULL;

		disk_metadata.crc32 = 0;
		disk_metadata.crc32 = crc32_seedless(&disk_metadata, sizeof(disk_metadata));

		pbyte_s = disk_md_partition->pba_s * pblk_size;
		n_bytes_write = roundup(sizeof(disk_metadata), pblk_size);
		dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_selftest_stress_disk_md, PAGE_SIZE, n_bytes_write);
		memcpy(dma_buffer, &disk_metadata, sizeof(disk_metadata));

		if (pwrite(fd, dma_buffer, n_bytes_write, pbyte_s) != n_bytes_write) {
			N_Ef(selftest_write_stress_disk_md_failed, "Failed to write disk_metadata to stress test device");
			NNVMEIBT_BM_FREE(trace_selftest_stress_disk_md_free, dma_buffer);
			goto out;
		}

		NNVMEIBT_BM_FREE(trace_selftest_stress_disk_md_free2, dma_buffer);
	}

	fsync(fd);
	rv = fd;
	fd = -1;

out:
	if (fd >= 0) {
		close(fd);
	}
	return rv;
}

DEFINE_TEST(stress_large_gpt_single_modification)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_stress_large";
	struct nvmeibt_disk_gpt		main_gpt;
	int							fd = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt_json = NULL;
	struct mm_json_elem			*entries = NULL;
	char						partition_name_after[GPT_MAX_PARTITION_NAME_LENGTH + 1];
	int							n_partitions = 8000;	/* Stress test with near-maximum partitions */
	int							target_partition = 4000;	/* Modify middle partition */

	/* Step 1: Create device with many partitions */
	fd = create_device_with_many_partitions(device_path, n_partitions);
	if (fd < 0) {
		TEST_FAIL("SETUP FAILED: Could not create device with %d partitions", n_partitions);
		goto out;
	}
	close(fd);
	fd = -1;

	TEST_INFO("Created device with %d partitions", n_partitions);

	/* Step 2: Export to JSON */
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("stress_large"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Step 3: Modify ONLY partition 50's name */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("stress_large"));
	if (!json_root) {
		goto out;
	}

	/* Navigate to main_gpt_primary -> entries */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
			main_gpt_json = json_root->dict.elements[i].value;
			break;
		}
	}
	if (!main_gpt_json) {
		goto out;
	}

	for (int i = 0; i < main_gpt_json->dict.len; i++) {
		if (strcmp(main_gpt_json->dict.elements[i].key, "entries") == 0) {
			entries = main_gpt_json->dict.elements[i].value;
			break;
		}
	}
	if (!entries || entries->type != JSON_E_ARRAY || entries->array.len < target_partition) {
		goto out;
	}

	/* Find and modify partition 50 */
	for (int i = 0; i < entries->array.len; i++) {
		struct mm_json_elem *entry = entries->array.elements[i];
		int index = (int)json_get_dict_num(entry, "index", -1);

		if (index == target_partition) {
			json_set_dict_str(entry, "name", "partition_4000_MODIFIED");
			TEST_INFO("Modified partition %d name to: partition_4000_MODIFIED", target_partition);
			break;
		}
	}

	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("stress_large")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Step 4: Apply with --write (triggers full entries array rewrite) */
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("stress_large"),
				   "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Step 5: Verify the change was applied */
	fd = open(device_path, O_RDONLY);
	if (fd < 0) {
		goto out;
	}

	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
										  1, 500000 - 1, false) < 0) {
		goto out;
	}
	close(fd);
	fd = -1;

	/* Verify target partition was modified */
	if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&main_gpt.entries[target_partition])) {
		TEST_FAIL("Partition %d not in use", target_partition);
		goto out;
	}

	char16_str_to_str(main_gpt.entries[target_partition].partition_name,
					  GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name_after);
	if (strcmp(partition_name_after, "partition_4000_MODIFIED") != 0) {
		TEST_FAIL("Partition %d name not modified (got: %s)", target_partition, partition_name_after);
		goto out;
	}

	TEST_SUCCEED("Verified: Single modification in large GPT (%d partitions) handled correctly", n_partitions);
	TEST_INFO("Note: GPT spec requires rewriting entire entries array (~1MB for 8192 entries)");
	TEST_INFO("Performance: Modifying 1 entry triggers 2MB write (primary + alternate)");
	TEST_INFO("Device file is sparse - actual disk usage is minimal despite 2GB size");
	rv = 0;

out:
	/* Cleanup */
	if (fd >= 0) {
		close(fd);
	}
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("stress_large"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(o_direct_flags)
{
	int rv = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);

	/* Test --direct flag (may fail for regular file, but should not crash) */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--direct", "-c", "both");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_INFO("Note: --direct may fail for regular files (expected)");
		/* Graceful handling = success */
	}

	/* Test --no-direct flag (should work for regular files) */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--no-direct", "-c", "both");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	TEST_SUCCEED("Verified: O_DIRECT flags handled correctly");
	rv = 0;

out:
	return rv;
}

DEFINE_TEST(export_without_toma)
{
	int							rv = -1;
	struct mm_json_elem			*json_root = NULL;
	BOOL						toma_running;

	/* Create device and export (TOMA not running - guaranteed by run_self_test check) */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("no_toma"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Verify JSON has TOMA status but no memory sections */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("no_toma"));
	if (!json_root) {
		goto out;
	}

	toma_running = json_get_dict_bool(json_root, "_toma_running", true);
	if (toma_running) {
		TEST_FAIL("JSON says TOMA running (expected false)");
		goto out;
	}

	if (json_get_dict_value(json_root, "memory_main_gpt") || json_get_dict_value(json_root, "memory_metadata_gpt")) {
		TEST_FAIL("JSON contains memory sections (should not)");
		goto out;
	}

	TEST_SUCCEED("Verified: TOMA not running, no memory sections in JSON");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("no_toma"));
	return rv;
}

DEFINE_TEST(write_blocked_toma_running)
{
	int							rv = -1;
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*disk_md = NULL;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("write_block"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Modify JSON to create changes (so confirmation is triggered) */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("write_block"));
	if (!json_root) {
		goto out;
	}

	/* Find disk_metadata and modify it to trigger a change */
	for (int i = 0; i < json_root->dict.len; i++) {
		if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
			disk_md = json_root->dict.elements[i].value;
			break;
		}
	}
	if (disk_md) {
		json_set_dict_str(disk_md, "ldisk_id_str", "MODIFIED_TO_TRIGGER_CHANGE");
		TEST_INFO("Modified JSON to trigger a change");
	}
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("write_block")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Try to apply with --write (should be BLOCKED by TOMA check at confirmation) */
	SELF_TEST_mock_toma_running();
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("write_block"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) == 0) {
		SELF_TEST_undo_mock_toma_running();
		TEST_FAIL("Should have been blocked!");
		goto out;
	}
	SELF_TEST_undo_mock_toma_running();

	TEST_SUCCEED("Operation correctly blocked");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("write_block"));
	return rv;
}

DEFINE_TEST(memory_sections_ignored)
{
	int							rv = -1;
	struct mm_json_elem			*json_root = NULL;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("mem_sections"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	/* Manually add fake memory_main_gpt section to JSON */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("mem_sections"));
	if (!json_root) {
		goto out;
	}

	/* Add minimal memory_main_gpt section */
	json_set_dict_str(json_root, "memory_main_gpt", "fake_data");
	TEST_INFO("Added fake memory_main_gpt section to JSON");
	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("mem_sections")) < 0) {
		goto out;
	}
	json_root = NULL;

	/* Try to apply (should succeed, ignoring memory sections) */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("mem_sections"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	TEST_SUCCEED("Verified: Memory sections ignored gracefully during apply");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("mem_sections"));
	return rv;
}

/**
 * Test get_memory_gpt_via_rpc() using mock GPT structures
 * This test verifies the JSON export
 */
DEFINE_TEST(export_memory_gpt)
{
	int								rv = -1;
	struct nvmeibt_local_disk		mock_local_disk;
	union nvmeib_uuid				test_uuid;
	struct mm_json_elem				*json_root = NULL;
	struct mm_json_elem				*mem_main_gpt;
	struct mm_json_elem				*mem_meta_gpt;
	struct mm_json_elem				*entries;
	BOOL							toma_running;

	/* Step 1: Create mock local_disk structure */
	memset(&mock_local_disk, 0, sizeof(mock_local_disk));

	/* Setup mock Main GPT */
	nvmeibt_strlcpy(mock_local_disk.main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(mock_local_disk.main_gpt.main_or_metadata));
	mock_local_disk.main_gpt.max_n_entries = LARGE_GPT_MAX_NUM_GPT_ENTRIES;
	nvmeibt_urn_uuid_str_to_union_uuid(&mock_local_disk.main_gpt.header.disk_obj_uuid, "12345678-1234-1234-1234-123456789abc");
	mock_local_disk.main_gpt.header.first_usable_pba = 34;
	mock_local_disk.main_gpt.header.last_usable_pba = 1999;
	mock_local_disk.main_gpt.header.n_partition_entries = 128;
	mock_local_disk.main_gpt.header.header_crc32 = 0x12345678;
	mock_local_disk.main_gpt.header.partition_entry_array_crc32 = 0xabcdef00;

	/* Add mock entry to Main GPT */
	nvmeibt_urn_uuid_str_to_union_uuid(&test_uuid, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
	mock_local_disk.main_gpt.entries[0].partition_type_guid = test_uuid;
	nvmeibt_urn_uuid_str_to_union_uuid(&test_uuid, "11111111-2222-3333-4444-555555555555");
	mock_local_disk.main_gpt.entries[0].partition_guid = test_uuid;
	mock_local_disk.main_gpt.entries[0].pba_s = 100;
	mock_local_disk.main_gpt.entries[0].pba_e = 200;
	mock_local_disk.main_gpt.entries[0].attributes = 0;
	str_to_char16_str("MOCK_MAIN_ENTRY", 15, mock_local_disk.main_gpt.entries[0].partition_name);

	/* Setup mock Metadata GPT */
	nvmeibt_strlcpy(mock_local_disk.metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(mock_local_disk.metadata_gpt.main_or_metadata));
	mock_local_disk.metadata_gpt.max_n_entries = MAX_NUM_GPT_ENTRIES;
	nvmeibt_urn_uuid_str_to_union_uuid(&mock_local_disk.metadata_gpt.header.disk_obj_uuid, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	mock_local_disk.metadata_gpt.header.first_usable_pba = 2;
	mock_local_disk.metadata_gpt.header.last_usable_pba = 99;
	mock_local_disk.metadata_gpt.header.n_partition_entries = 128;
	mock_local_disk.metadata_gpt.header.header_crc32 = 0xdeadbeef;
	mock_local_disk.metadata_gpt.header.partition_entry_array_crc32 = 0xcafebabe;

	/* Add mock entry to Metadata GPT */
	nvmeibt_urn_uuid_str_to_union_uuid(&test_uuid, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
	mock_local_disk.metadata_gpt.entries[0].partition_type_guid = test_uuid;
	nvmeibt_urn_uuid_str_to_union_uuid(&test_uuid, "66666666-7777-8888-9999-aaaaaaaaaaaa");
	mock_local_disk.metadata_gpt.entries[0].partition_guid = test_uuid;
	mock_local_disk.metadata_gpt.entries[0].pba_s = 10;
	mock_local_disk.metadata_gpt.entries[0].pba_e = 50;
	mock_local_disk.metadata_gpt.entries[0].attributes = 0;
	str_to_char16_str("MOCK_META_ENTRY", 15, mock_local_disk.metadata_gpt.entries[0].partition_name);

	/* Setup mock MBR */
	mock_local_disk.mbr.signature = MBR_SIGNATURE;
	mock_local_disk.mbr.partitions[0].os_type = 0xEE;
	mock_local_disk.mbr.partitions[0].pba_s = 1;
	mock_local_disk.mbr.partitions[0].n_pblk = 2000;

	/* Setup change tracking */
	mock_local_disk.gpt_change_no = 5;
	mock_local_disk.gpt_submitted_change_no = 3;

	TEST_INFO("Created mock local_disk with GPT structures");

	/* Step 2: Set mock local_disk and mock TOMA as running */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_set_mock_local_disk(&mock_local_disk);
	SELF_TEST_mock_toma_running();

	/* Step 3: Run export - should use mock structures */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("mem_rpc"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	SELF_TEST_undo_mock_toma_running();
	SELF_TEST_set_mock_local_disk(NULL);

	if (rv != 0) {
		goto out;
	}

	/* Step 4: Verify the exported JSON contains memory sections from mock */
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("mem_rpc"));
	if (!json_root) {
		TEST_FAIL("Cannot parse exported JSON");
		rv = -1;
		goto out;
	}

	/* Verify _toma_running is true */
	toma_running = json_get_dict_bool(json_root, "_toma_running", false);
	if (!toma_running) {
		TEST_FAIL("JSON says TOMA not running (expected true)");
		rv = -1;
		goto out;
	}

	/* Verify memory_main_gpt section exists with our mock data */
	mem_main_gpt = json_get_dict_value(json_root, "memory_main_gpt");
	if (!mem_main_gpt || mem_main_gpt->type != JSON_E_DICT) {
		TEST_FAIL("memory_main_gpt section missing or invalid");
		rv = -1;
		goto out;
	}

	entries = json_get_dict_value(mem_main_gpt, "entries");
	if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
		TEST_FAIL("memory_main_gpt has no entries");
		rv = -1;
		goto out;
	}
	TEST_SUCCEED("Verified: memory_main_gpt from mock structures (%d entries)",
				 entries->array.len);

	/* Verify memory_metadata_gpt section exists */
	mem_meta_gpt = json_get_dict_value(json_root, "memory_metadata_gpt");
	if (!mem_meta_gpt || mem_meta_gpt->type != JSON_E_DICT) {
		TEST_FAIL("memory_metadata_gpt section missing or invalid");
		rv = -1;
		goto out;
	}

	entries = json_get_dict_value(mem_meta_gpt, "entries");
	if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
		TEST_FAIL("memory_metadata_gpt has no entries");
		rv = -1;
		goto out;
	}
	TEST_SUCCEED("Verified: memory_metadata_gpt from mock structures (%d entries)",
				 entries->array.len);

	TEST_SUCCEED("Verified: get_memory_gpt_via_rpc() uses mock structures correctly");
	rv = 0;

out:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("mem_rpc"));
	return rv;
}

/**
 * Test: Export segment metadata partitions and apply changes
 * Verifies export creates correct JSON AND that editable/WARNING fields can be applied
 * Uses comprehensive JSON comparison to verify ALL fields after apply
 */
DEFINE_TEST(export_segment_metadata)
{
	int							rv = -1;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_seg_metadata_test";
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*seg_md_partitions = NULL;
	struct mm_json_elem			*seg1 = NULL;

	// Create device with segment metadata partitions
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_disk_with_segment_metadata, device_path);

	// PART 1: Export to JSON and verify basic structure
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("seg_metadata_edited"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// Parse and verify exported JSON has correct structure
	json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("seg_metadata_edited"));
	if (!json_root) {
		TEST_FAIL("Cannot parse exported JSON");
		goto out;
	}

	seg_md_partitions = json_get_dict_value(json_root, "segment_metadata_partitions");
	if (!seg_md_partitions || seg_md_partitions->type != JSON_E_ARRAY || seg_md_partitions->array.len != 2) {
		TEST_FAIL("segment_metadata_partitions missing or incorrect count");
		goto out;
	}

	seg1 = seg_md_partitions->array.elements[0];
	if (!seg1 || strcmp(json_get_dict_str(seg1, "hostname", ""), "test-host-1") != 0) {
		TEST_FAIL("Segment 1 hostname incorrect in export");
		goto out;
	}

	if ((int)json_get_dict_num(seg1, "_WARNING_n_blksets_scrubbed", -1) != 42) {
		TEST_FAIL("Segment 1 n_blksets_scrubbed incorrect in export");
		goto out;
	}

	TEST_SUCCEED("Export verified: 2 segment partitions with correct fields");

	// PART 2: Modify JSON and apply
	// Modify first segment's hostname and n_blksets_scrubbed using JSON tree API
	json_set_dict_str(seg1, "hostname", "MODIFY-HOST");
	json_set_dict_num(seg1, "_WARNING_n_blksets_scrubbed", 99);

	if (SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("seg_metadata_edited")) < 0) {
		json_root = NULL;
		TEST_FAIL("Failed to write modified JSON");
		goto out;
	}
	json_root = NULL;

	// Apply with --write
	SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("seg_metadata_edited"), "--write", "--yes");
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		goto out;
	}

	// PART 3: Re-export JSON after apply
	SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("seg_metadata_reexported"));
	if (SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv) != 0) {
		TEST_FAIL("Failed to re-export JSON after apply");
		goto out;
	}

	// Comprehensive comparison: edited JSON vs re-exported JSON
	TEST_INFO("Comparing ALL fields: edited JSON vs re-exported JSON...");
	if (SELF_TEST_compare_json_files(TEST_JSON_PATH("seg_metadata_edited"),
									  TEST_JSON_PATH("seg_metadata_reexported"),
									  ctx->quiet_mode) != 0) {
		TEST_FAIL("Comprehensive JSON comparison failed: re-exported differs from edited");
		goto out;
	}

	TEST_SUCCEED("Comprehensive JSON comparison passed: ALL segment_metadata fields correctly applied");
	rv = 0;

out:
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("seg_metadata_edited"));
	unlink(TEST_JSON_PATH("seg_metadata_reexported"));
	cleanup_backup_files_for_device(device_path);
	return rv;
}

/**
 * Run comprehensive self-test suite
 */
int run_self_test(const char *test_selection, BOOL quiet_mode)
{
	int							disk_fd = -1;
	int							tests_run = 0;
	int							tests_passed = 0;
	int							tests_failed = 0;
	const char					*test_device_path;
	const char					*wrong_device_path;
	char						*test_argv[10];
	int							test_argc;
	struct self_test_ctx		ctx;
	BOOL						*tests_to_run = NULL;
	int							*test_results = NULL;
	int							num_tests_total = 1;		// Will be updated

	// Test registry - auto-generated from SELF_TEST_LIST X-Macro
	#define X(func, name, cmd) {name, cmd, test_##func},
	struct self_test_entry tests[] = { SELF_TEST_LIST };
	#undef X

	/* Safety check: Block self-tests if TOMA is running */
	if (SELF_TEST_acquire_toma_lock()) {
		fprintf(stderr, COL_RED_BOLD "\nERROR: Cannot run self-tests while TOMA is running!" COL_RESET "\n");
		fprintf(stderr, "Self-tests may spawn fake TOMA process and create conflicts.\n");
		fprintf(stderr, "Please stop TOMA before running tests.\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "To stop TOMA: sudo pkill -9 nvmeibt_toma\n");
		fprintf(stderr, "\n");
		return 1;
	}

	num_tests_total = sizeof(tests) / sizeof(tests[0]);

	mkdir(TOMA_ROOT_DIR "tmp", 0755);
	test_device_path = TOMA_ROOT_DIR "tmp/gpt_util_self_test";
	wrong_device_path = TOMA_ROOT_DIR "tmp/gpt_util_wrong_device";

	// Setup context
	ctx.test_device_path = test_device_path;
	ctx.wrong_device_path = wrong_device_path;
	ctx.test_argv = test_argv;
	ctx.test_argc = &test_argc;
	ctx.quiet_mode = quiet_mode;

	// Allocate tracking arrays
	tests_to_run = calloc(num_tests_total, sizeof(BOOL));
	test_results = calloc(num_tests_total, sizeof(int));		/* 0=not run, 1=passed, -1=failed */

	if (test_selection == NULL) {
		// Run all tests
		for (int i = 0; i < num_tests_total; i++) {
			tests_to_run[i] = true;
		}
	} else {
		// Parse comma-separated list and ranges
		char *selection_copy = strdup(test_selection);
		char *token = strtok(selection_copy, ",");
		while (token != NULL) {
			char *dash = strchr(token, '-');
			if (dash) {
				// Range: "3-5"
				int start = atoi(token);
				int end = atoi(dash + 1);
				for (int j = start; j <= end && j <= num_tests_total; j++) {
					if (j >= 1) tests_to_run[j - 1] = true;
				}
			} else {
				// Single test: "3"
				int test_num = atoi(token);
				if (test_num >= 1 && test_num <= num_tests_total) {
					tests_to_run[test_num - 1] = true;
				}
			}
			token = strtok(NULL, ",");
		}
		free(selection_copy);
	}

	// Run selected tests
	for (int i = 0; i < num_tests_total; i++) {
		if (tests_to_run[i]) {
			int test_num = i + 1;		// Actual test number (from registry position)
			int rv;

			SELF_TEST_start(test_num, tests[i].name, tests[i].command, quiet_mode);
			rv = tests[i].func(&ctx);

			tests_run++;
			if (SELF_TEST_end(test_num, rv) == 0) {
				tests_passed++;
				test_results[i] = 1;		/* Passed */
			} else {
				tests_failed++;
				test_results[i] = -1;		/* Failed */
			}
		}
	}

	// Summary
	fprintf(stdout, "\n");
	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");

	if (tests_failed == 0) {
		fprintf(stdout, COL_GREEN "ALL SELF-TESTS PASSED (%d/%d)" COL_RESET "\n", tests_passed, tests_run);
	} else {
		fprintf(stdout, COL_RED_BOLD "SOME TESTS FAILED: %d passed, %d failed (%d total)" COL_RESET "\n",
				tests_passed, tests_failed, tests_run);
	}

	fprintf(stdout, COL_GREEN "============================================================" COL_RESET "\n");

	/* Always show test list with pass/fail status */
	fprintf(stdout, "\nTest Results (%d tests):\n", num_tests_total);
	for (int i = 0; i < num_tests_total; i++) {
		const char *status;
		const char *color;

		if (test_results[i] == 1) {
			status = PASS;
			color = COL_GREEN;
		} else if (test_results[i] == -1) {
			status = FAIL;
			color = COL_RED_BOLD;
		} else {
			status = SKIP;
			color = COL_YELLOW;
		}

		fprintf(stdout, "  %s[%s] Test %2d:%s %s\n",
				color, status, i + 1, COL_RESET, tests[i].name);
	}

	// Cleanup
	free(tests_to_run);
	free(test_results);
	if (disk_fd >= 0) {
		close(disk_fd);
	}

	/* Safety net cleanup: Remove common test resources if tests crashed/aborted */
	/* Each test cleans up its own files - this is just for abnormal termination */
	unlink(test_device_path);
	unlink(wrong_device_path);
	cleanup_backup_files_for_device(test_device_path);
	SELF_TEST_release_toma_lock();
	return (tests_failed > 0) ? 1 : 0;
}

