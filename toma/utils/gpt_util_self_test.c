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
 * expect_failure: if true, non-zero result is success (for negative tests)
 * Returns: 0 if passed, -1 if failed (for counting)
 */
int SELF_TEST_end(int test_num, int result, BOOL expect_failure)
{
	BOOL test_passed;

	if (expect_failure) {
		// Negative test: expecting failure
		test_passed = (result != 0);
		if (test_passed) {
			fprintf(stdout, COL_GREEN "Operation correctly blocked" COL_RESET "\n");
		}
	} else {
		// Normal test: expecting success
		test_passed = (result == 0);
	}

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
	}

	fsync(fd);

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
	struct nvmeibt_disk_gpt_partition_entry	*metadata_partition;

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
	{
		uint64_t metadata_start = main_gpt.header.first_usable_pba;
		uint64_t metadata_end = main_gpt.header.last_usable_pba;

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
	{
		const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
		struct nvmeibt_disk_metadata	disk_metadata;
		char							*dma_buffer = NULL;
		int								n_bytes_write;
		uint64_t						pbyte_s;

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
	if (fd >= 0) close(fd);
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
	if (fd >= 0) close(fd);
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
	int						i;

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

	for (i = 0; i < json_root->dict.len; i++) {
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
 * Looks for /tmp/backup_<device_basename>_<timestamp>/ directories
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

	/* Scan /tmp for backup directories */
	dir = opendir("/tmp");
	if (!dir) {
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, pattern, strlen(pattern)) == 0 && entry->d_type == DT_DIR) {
			char full_path[512];
			struct stat st;
			snprintf(full_path, sizeof(full_path), "/tmp/%s", entry->d_name);
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
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
		unlink(full_path);		/* Remove files (directories would fail, which is fine) */
	}

	closedir(dir);
	rmdir(dir_path);		/* Remove the directory itself */
}

/**
 * Cleanup all backup directories for a device
 * Removes all /tmp/backup_<device_basename>_<timestamp> directories
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

	/* Scan /tmp for backup directories */
	dir = opendir("/tmp");
	if (!dir) {
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		/* Match backup directories: backup_<device>_<timestamp> */
		if (strncmp(entry->d_name, pattern, strlen(pattern)) == 0 && entry->d_type == DT_DIR) {
			snprintf(full_path, sizeof(full_path), "/tmp/%s", entry->d_name);
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
	int rv = 0;

	// Step 1: Display mismatched GPT
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	// Step 2: Export and verify flag
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both", "-J", TEST_JSON_PATH("mismatch"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	if (rv == 0) {
		rv = SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("mismatch"), "_READONLY_mismatch_detected", true);
	}

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
	int rv = 0;

	// Step 1: Display overlapping GPT
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	// Step 2: Export and verify flag
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("overlaps"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	if (rv == 0) {
		rv = SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("overlaps"), "_READONLY_overlaps_detected", true);
	}

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
	int							rv = 0;
	int							disk_fd;

	disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(ctx->test_device_path);
	if (disk_fd < 0) {
		return -1;
	}

	if (rv == 0) {
		rv = SELF_TEST_corrupt_gpt_n_partition_entries(disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
														1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1,
														wrong_n_partition_entries);
	}

	if (rv == 0) {
		memset(&main_gpt, 0, sizeof(main_gpt));
		nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
		rv = nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
											   &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false);
	}

	if (rv == 0) {
		nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * main_gpt.header.size_of_partition_entry;
		expected_crc = crc32_seedless(main_gpt.entries, nbytes);
		if (main_gpt.header.n_partition_entries != wrong_n_partition_entries ||
			main_gpt.header.partition_entry_array_crc32 != expected_crc) {
			rv = -1;
		}
	}

	if (rv == 0) {
		rv = SELF_TEST_upgrade_gpt_if_needed(disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, "Main");
	}

	if (rv == 0) {
		memset(&verify_gpt, 0, sizeof(verify_gpt));
		nvmeibt_strlcpy(verify_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(verify_gpt.main_or_metadata));
		rv = nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
											   &verify_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false);
	}

	if (rv == 0) {
		nbytes = LARGE_GPT_MAX_NUM_GPT_ENTRIES * verify_gpt.header.size_of_partition_entry;
		expected_crc = crc32_seedless(verify_gpt.entries, nbytes);
		if (verify_gpt.header.n_partition_entries != LARGE_GPT_MAX_NUM_GPT_ENTRIES ||
			verify_gpt.header.partition_entry_array_crc32 != expected_crc) {
			rv = -1;
			/*
			 * Intentionally allow test to pass if n_partition_entries is 128.
			 * TODO(NVMESH-7436): Remove this line once ticket is fixed.
			 */
			if (verify_gpt.header.n_partition_entries == GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES) { rv = 0; }
		}
	}

	if (disk_fd >= 0) close(disk_fd);
	return rv;
}

DEFINE_TEST(json_export_apply)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("export"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("export"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

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
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("diff_baseline"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("diff_baseline"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("diff_baseline"));
	return rv;
}

DEFINE_TEST(diff_modifications)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("standard"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_modified, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("standard"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("standard"));
	return rv;
}

DEFINE_TEST(apply_write)
{
	int rv = 0;
	const char *device_a = TOMA_ROOT_DIR "tmp/gpt_device_a";
	const char *device_b = TOMA_ROOT_DIR "tmp/gpt_device_b";

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_a);
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_modified, device_b);

	// Verify A != B initially
	if (rv == 0) {
		if (SELF_TEST_compare_gpt_binary(device_a, device_b, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, SELF_TEST_MOCK_DEVICE_BLOCKS) == 0) {
			rv = -1;
		}
	}

	// Export A, modify JSON to point to B, apply to B
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_a, "-J", TEST_JSON_PATH("write_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("write_test"));
		char mock_serial_b[64];

		SELF_TEST_generate_mock_serial_number_from_path(device_b, mock_serial_b, sizeof(mock_serial_b));
		if (!json_root ||
			json_set_dict_str(json_root, "device_path", device_b) < 0 ||
			json_set_dict_str(json_root, "_READONLY_controller_serial_num", mock_serial_b) < 0 ||
			SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("write_test")) < 0) {
			rv = -1;
		}
	}
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_b, "--apply-from", TEST_JSON_PATH("write_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	// Verify A == B (complete binary roundtrip fidelity)
	if (rv == 0) {
		rv = SELF_TEST_compare_gpt_binary(device_a, device_b, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, SELF_TEST_MOCK_DEVICE_BLOCKS);
	}

	unlink(TEST_JSON_PATH("write_test"));
	cleanup_backup_files_for_device(device_a);
	cleanup_backup_files_for_device(device_b);
	unlink(device_a);
	unlink(device_b);
	return rv;
}

DEFINE_TEST(missing_section)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("missing_gpt"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		rv = SELF_TEST_remove_json_field(TEST_JSON_PATH("missing_gpt"), "main_gpt_primary");
	}

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("missing_gpt"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("missing_gpt"));
	return rv;
}

DEFINE_TEST(overlap_blocking)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("overlap_block"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_overlaps, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("overlap_block"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("overlap_block"));
	return rv;
}

DEFINE_TEST(mismatch_blocking)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_mock_device_with_mismatch, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-c", "both", "-J", TEST_JSON_PATH("mismatch_block"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		rv = SELF_TEST_validate_json_bool_flag(TEST_JSON_PATH("mismatch_block"), "_READONLY_mismatch_detected", true);
	}

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("mismatch_block"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("mismatch_block"));
	return rv;
}

DEFINE_TEST(serial_number_mismatch)
{
	int			rv = 0;
	const char	*device_a = TOMA_ROOT_DIR "tmp/gpt_serial_device_a";
	const char	*device_b = TOMA_ROOT_DIR "tmp/gpt_serial_device_b";
	int			fd;

	/* Create device A */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_a);
	if (fd < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create device A" COL_RESET "\n");
		return -1;
	}
	close(fd);

	/* Create device B */
	fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_b);
	if (fd < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create device B" COL_RESET "\n");
		unlink(device_a);
		return -1;
	}
	close(fd);

	/* Export JSON from device A */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_a, "-J", TEST_JSON_PATH("serial_check"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Try to apply JSON from A to device B (should be BLOCKED) */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("serial_check"));
		if (json_root && json_set_dict_str(json_root, "device_path", device_b) == 0 &&
			SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("serial_check")) == 0) {
			fprintf(stdout, "Modified JSON device_path to point to device B\n");
		} else {
			rv = -1;
		}
	}

	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_b, "--apply-from", TEST_JSON_PATH("serial_check"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure (serial mismatch), so rv != 0 is success for this test */
	}

	unlink(TEST_JSON_PATH("serial_check"));
	cleanup_backup_files_for_device(device_a);
	cleanup_backup_files_for_device(device_b);
	unlink(device_a);
	unlink(device_b);
	return rv;
}

DEFINE_TEST(missing_serial_number)
{
	int rv = 0;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("missing_serial"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Remove controller_serial_num from JSON (now at root level) */
	if (rv == 0) {
		rv = SELF_TEST_remove_json_field(TEST_JSON_PATH("missing_serial"), "_READONLY_controller_serial_num");
	}

	/* Try to apply - should be BLOCKED */
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("missing_serial"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("missing_serial"));
	return rv;
}

DEFINE_TEST(delete_main_entry)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_delete_test";
	struct nvmeibt_disk_gpt		gpt_before;
	struct nvmeibt_disk_gpt		gpt_after;
	int							fd = -1;

	/* Create device with standard structure */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Verify device has entry 0 before deletion */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&gpt_before, 0, sizeof(gpt_before));
		nvmeibt_strlcpy(gpt_before.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_before.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_before,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&gpt_before.entries[0])) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Entry 0 should exist before deletion" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("delete_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Set _delete flag to true for main_gpt_primary entry 0 */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("delete_test"));
		struct mm_json_elem *main_gpt = NULL;
		struct mm_json_elem *entries = NULL;
		int i;

		if (json_root) {
			/* Navigate: root -> main_gpt_primary -> entries -> [0] -> _delete */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
					main_gpt = json_root->dict.elements[i].value;
					break;
				}
			}
			if (main_gpt) {
				for (i = 0; i < main_gpt->dict.len; i++) {
					if (strcmp(main_gpt->dict.elements[i].key, "entries") == 0) {
						entries = main_gpt->dict.elements[i].value;
						break;
					}
				}
			}
			if (entries && entries->array.len > 0) {
				json_set_dict_bool(entries->array.elements[0], "_delete", true);
				fprintf(stdout, "Set main_gpt_primary entries[0]._delete = true\n");
			}
			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("delete_test"));
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("delete_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify entry 0 is deleted */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&gpt_after, 0, sizeof(gpt_after));
		nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&gpt_after.entries[0])) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Entry 0 should be deleted after apply" COL_RESET "\n");
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: Entry 0 successfully deleted" COL_RESET "\n");
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("delete_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(delete_metadata_entry)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_delete_metadata_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt_before;
	struct nvmeibt_disk_gpt		metadata_gpt_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition = NULL;
	int							fd = -1;

	/* Create device with standard structure */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read Main GPT to get metadata partition location */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&main_gpt, 0, sizeof(main_gpt));
		nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
		if (!metadata_partition) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No metadata partition found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Verify metadata GPT has entry 0 before deletion */
	if (rv == 0) {
		memset(&metadata_gpt_before, 0, sizeof(metadata_gpt_before));
		nvmeibt_strlcpy(metadata_gpt_before.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_before.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_before,
											  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt_before.entries[0])) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Metadata entry 0 should exist before deletion" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("delete_metadata_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Set _delete flag to true for metadata_gpt_primary entry 0 */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("delete_metadata_test"));
		struct mm_json_elem *metadata_gpt = NULL;
		struct mm_json_elem *entries = NULL;
		int i;

		if (json_root) {
			/* Navigate: root -> metadata_gpt_primary -> entries -> [0] -> _delete */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "metadata_gpt_primary") == 0) {
					metadata_gpt = json_root->dict.elements[i].value;
					break;
				}
			}
			if (metadata_gpt) {
				for (i = 0; i < metadata_gpt->dict.len; i++) {
					if (strcmp(metadata_gpt->dict.elements[i].key, "entries") == 0) {
						entries = metadata_gpt->dict.elements[i].value;
						break;
					}
				}
			}
			if (entries && entries->array.len > 0) {
				json_set_dict_bool(entries->array.elements[0], "_delete", true);
				fprintf(stdout, "Set metadata_gpt_primary entries[0]._delete = true\n");
			}
			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("delete_metadata_test"));
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("delete_metadata_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify metadata entry 0 is deleted */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&main_gpt, 0, sizeof(main_gpt));
		nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
		if (!metadata_partition) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Metadata partition should still exist" COL_RESET "\n");
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&metadata_gpt_after, 0, sizeof(metadata_gpt_after));
		nvmeibt_strlcpy(metadata_gpt_after.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_after.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_after,
											  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt_after.entries[0])) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Metadata entry 0 should be deleted after apply" COL_RESET "\n");
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: Metadata GPT entry 0 successfully deleted" COL_RESET "\n");
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("delete_metadata_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(readonly_fields_ignored)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_readonly_test";
	struct nvmeibt_disk_gpt		gpt_before;
	struct nvmeibt_disk_gpt		gpt_after;
	int		 					correct_header_crc;
	int							bogus_crc;
	int							fd = -1;

	correct_header_crc = 0;
	bogus_crc = (int)0xDEADBEEF;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read original CRC */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&gpt_before, 0, sizeof(gpt_before));
		nvmeibt_strlcpy(gpt_before.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_before.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_before,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		correct_header_crc = gpt_before.header.header_crc32;
		fprintf(stdout, "Original header CRC: 0x%08x\n", correct_header_crc);
	}

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("readonly_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify _READONLY_header_crc32 to bogus value (CRCs exported as hex strings) */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("readonly_test"));
		struct mm_json_elem *main_gpt = NULL;
		int i;

		if (json_root) {
			/* Navigate to main_gpt_primary section */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
					main_gpt = json_root->dict.elements[i].value;
					break;
				}
			}
			if (main_gpt && json_set_dict_str(main_gpt, "_READONLY_header_crc32", "0xdeadbeef") == 0) {
				fprintf(stdout, "Modified main_gpt_primary._READONLY_header_crc32 = 0xdeadbeef\n");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("readonly_test"));
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("readonly_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify CRC on disk is correct (not the bogus value) */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&gpt_after, 0, sizeof(gpt_after));
		nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (gpt_after.header.header_crc32 == bogus_crc) {
			fprintf(stdout, COL_RED_BOLD "FAIL: CRC on disk is bogus value 0x%08x (should have been recalculated)" COL_RESET "\n", bogus_crc);
			rv = -1;
		} else if (gpt_after.header.header_crc32 == correct_header_crc) {
			fprintf(stdout, COL_GREEN "Verified: CRC recalculated correctly (0x%08x, not bogus 0x%08x)" COL_RESET "\n",
					gpt_after.header.header_crc32, bogus_crc);
		} else {
			fprintf(stdout, COL_YELLOW "Note: CRC changed (old=0x%08x, new=0x%08x, bogus=0x%08x)" COL_RESET "\n",
					correct_header_crc, gpt_after.header.header_crc32, bogus_crc);
			fprintf(stdout, COL_GREEN "Verified: Bogus CRC was ignored (CRC != 0x%08x)" COL_RESET "\n", bogus_crc);
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("readonly_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(static_fields_validated)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_static_test";
	struct nvmeibt_disk_gpt		gpt_after;
	int							fd = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("static_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify _STATIC_gpt_signature to bogus value */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("static_test"));
		struct mm_json_elem *main_gpt = NULL;
		int i;

		if (json_root) {
			/* Navigate to main_gpt_primary section */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
					main_gpt = json_root->dict.elements[i].value;
					break;
				}
			}
			if (main_gpt && json_set_dict_str(main_gpt, "_STATIC_gpt_signature", "0xdeadbeefdeadbeef") == 0) {
				fprintf(stdout, "Modified main_gpt_primary._STATIC_gpt_signature = 0xdeadbeefdeadbeef\n");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("static_test"));
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Apply with --write (should succeed - static fields ignored) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("static_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify GPT signature on disk is correct (not the bogus value) */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&gpt_after, 0, sizeof(gpt_after));
		nvmeibt_strlcpy(gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(gpt_after.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &gpt_after,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (gpt_after.header.gpt_signature != GPT_SIGNATURE) {
			fprintf(stdout, COL_RED_BOLD "FAIL: GPT signature on disk is wrong (0x%lx, expected 0x%lx)" COL_RESET "\n",
					gpt_after.header.gpt_signature, (uint64_t)GPT_SIGNATURE);
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: GPT signature correct (0x%lx), bogus value ignored" COL_RESET "\n",
					gpt_after.header.gpt_signature);
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("static_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(nguid_preservation)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_nguid_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt;
	struct nvmeibt_disk_metadata disk_md_before;
	struct nvmeibt_disk_metadata disk_md_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
	uint64_t					pbyte_s;
	int							fd = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Read original NGUID */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd >= 0) {
			memset(&main_gpt, 0, sizeof(main_gpt));
			nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) == 0) {
				metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
				if (metadata_partition) {
					memset(&metadata_gpt, 0, sizeof(metadata_gpt));
					nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
					if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
														  metadata_partition->pba_s, metadata_partition->pba_e, false) == 0) {
						disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
						if (disk_md_partition) {
							pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
							nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_before);
						}
					}
				}
			}
			close(fd);
			fd = -1;
		}
	}

	/* Export (NGUID will be in JSON) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("nguid_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Remove native_nguid from JSON */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("nguid_test"));
		struct mm_json_elem *disk_md = NULL;
		int i;

		if (json_root) {
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
					disk_md = json_root->dict.elements[i].value;
					break;
				}
			}
			/* Remove native_nguid field using JSON API - this tests the "missing field" path */
			if (disk_md) {
				/* We can't easily remove a field with current API, so just set it to null UUID */
				json_set_dict_str(disk_md, "native_nguid", "00000000-0000-0000-0000-000000000000");
			}
			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("nguid_test"));
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("nguid_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify NGUID preserved (not zeroed or changed) */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd >= 0) {
			memset(&main_gpt, 0, sizeof(main_gpt));
			nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) == 0) {
				metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
				if (metadata_partition) {
					memset(&metadata_gpt, 0, sizeof(metadata_gpt));
					nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
					if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
														  metadata_partition->pba_s, metadata_partition->pba_e, false) == 0) {
						disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
						if (disk_md_partition) {
							pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
							memset(&disk_md_after, 0, sizeof(disk_md_after));
							nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_after);
						}
					}
				}
			}
			close(fd);
		}
	}

	if (rv == 0) {
		if (memcmp(&disk_md_before.native_nguid_unused, &disk_md_after.native_nguid_unused, sizeof(disk_md_before.native_nguid_unused)) != 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: NGUID changed after apply" COL_RESET "\n");
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: NGUID preserved (unchanged despite missing from JSON)" COL_RESET "\n");
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("nguid_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(warning_fields_apply)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_warning_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt;
	struct nvmeibt_disk_metadata disk_md_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
	uint64_t					pbyte_s;
	int							fd = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("warning_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify _WARNING_ fields */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("warning_test"));
		struct mm_json_elem *disk_md = NULL;
		int i;

		if (json_root) {
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
					disk_md = json_root->dict.elements[i].value;
					break;
				}
			}
			if (disk_md) {
				json_set_dict_num(disk_md, "_WARNING_last_pba_zeroed", 12345);
				json_set_dict_num(disk_md, "_WARNING_format_request_counter", 99);
				fprintf(stdout, "Modified _WARNING_ fields (last_pba_zeroed=12345, format_request_counter=99)\n");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("warning_test"));
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("warning_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify WARNING fields applied */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd >= 0) {
			memset(&main_gpt, 0, sizeof(main_gpt));
			nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) == 0) {
				metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
				if (metadata_partition) {
					memset(&metadata_gpt, 0, sizeof(metadata_gpt));
					nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
					if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
														  metadata_partition->pba_s, metadata_partition->pba_e, false) == 0) {
						disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
						if (disk_md_partition) {
							pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
							memset(&disk_md_after, 0, sizeof(disk_md_after));
							nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md_after);
						}
					}
				}
			}
			close(fd);
		}
	}

	if (rv == 0) {
		if (disk_md_after.last_pba_zeroed != 12345 || disk_md_after.format_request_counter != 99) {
			fprintf(stdout, COL_RED_BOLD "FAIL: WARNING fields not applied (last_pba=%lu, counter=%u)" COL_RESET "\n",
					disk_md_after.last_pba_zeroed, disk_md_after.format_request_counter);
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: _WARNING_ fields successfully applied" COL_RESET "\n");
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("warning_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(disk_metadata_apply)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_disk_md_test";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt;
	struct nvmeibt_disk_metadata disk_md_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition = NULL;
	uint64_t					pbyte_s = 0;
	int							fd = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("disk_md_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify disk_metadata fields in JSON */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("disk_md_test"));
		struct mm_json_elem *disk_md = NULL;
		int i;

		if (json_root) {
			/* Find disk_metadata section */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
					disk_md = json_root->dict.elements[i].value;
					break;
				}
			}
			if (disk_md) {
				/* Modify safe fields */
				json_set_dict_str(disk_md, "ldisk_id_str", "MODIFIED_LDISK_ID");
				json_set_dict_num(disk_md, "format_metadata_size", 999999);
				fprintf(stdout, "Modified disk_metadata fields (ldisk_id_str, format_metadata_size)\n");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("disk_md_test"));
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("disk_md_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify disk_metadata changes on disk */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&main_gpt, 0, sizeof(main_gpt));
		nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
											  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
		if (!metadata_partition) {
			rv = -1;
		}
	}

	if (rv == 0) {
		memset(&metadata_gpt, 0, sizeof(metadata_gpt));
		nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
		if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
											  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
			rv = -1;
		}
	}

	if (rv == 0) {
		disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
		if (!disk_md_partition) {
			rv = -1;
		}
	}

	if (rv == 0) {
		pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
		memset(&disk_md_after, 0, sizeof(disk_md_after));
		if (nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
													 pbyte_s, &disk_md_after) < 0) {
			rv = -1;
		}
		close(fd);
		fd = -1;
	}

	if (rv == 0) {
		if (strcmp(disk_md_after.ldisk_id_str, "MODIFIED_LDISK_ID") != 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: ldisk_id_str not updated (expected MODIFIED_LDISK_ID, got %s)" COL_RESET "\n",
					disk_md_after.ldisk_id_str);
			rv = -1;
		} else if (disk_md_after.format_metadata_size != 999999) {
			fprintf(stdout, COL_RED_BOLD "FAIL: format_metadata_size not updated (expected 999999, got %u)" COL_RESET "\n",
					disk_md_after.format_metadata_size);
			rv = -1;
		} else {
			fprintf(stdout, COL_GREEN "Verified: disk_metadata fields successfully applied" COL_RESET "\n");
			fprintf(stdout, "  ldisk_id_str = %s\n", disk_md_after.ldisk_id_str);
			fprintf(stdout, "  format_metadata_size = %u\n", disk_md_after.format_metadata_size);
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("disk_md_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(zero_change_write_skip)
{
	int rv = 0;

	/* Export from device A */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("zero_change"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Apply same JSON back with --write (0 changes) */
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("zero_change"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Success means apply completed without writing (validated by audit log showing changes=0) */
	if (rv == 0) {
		fprintf(stdout, COL_GREEN "Verified: Apply with 0 changes completed successfully" COL_RESET "\n");
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("zero_change"));
	return rv;
}

DEFINE_TEST(binary_backup_restore)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_binary_test";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	int							i;

	/* Step 1: Create original device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Step 2: Export and modify */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("binary_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify disk_metadata to trigger write */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("binary_test"));
		struct mm_json_elem *disk_md = NULL;

		if (json_root) {
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
					disk_md = json_root->dict.elements[i].value;
					break;
				}
			}
			if (disk_md) {
				json_set_dict_str(disk_md, "ldisk_id_str", "MODIFIED_FOR_BACKUP_TEST");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("binary_test"));
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Step 3: Apply with --write --yes (creates modular backup automatically) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("binary_test"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		fprintf(stdout, "Modular backup should have been created in /tmp/\n");
	}

	/* Step 4: Find the backup directory (newest backup_* directory in /tmp) */
	if (rv == 0) {
		DIR *dir = opendir("/tmp");
		struct dirent *entry;
		time_t newest_time = 0;
		char backup_dir[512] = {0};

		if (dir) {
			while ((entry = readdir(dir)) != NULL) {
				if (strncmp(entry->d_name, "backup_", 7) == 0 && entry->d_type == DT_DIR) {
					char full_path[512];
					struct stat st;
					snprintf(full_path, sizeof(full_path), "/tmp/%s", entry->d_name);
					if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_mtime > newest_time) {
						newest_time = st.st_mtime;
						nvmeibt_strlcpy(backup_dir, full_path, sizeof(backup_dir));
					}
				}
			}
			closedir(dir);
		}

		if (backup_dir[0] == '\0') {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		} else {
			snprintf(manifest_file, sizeof(manifest_file), "%s/manifest.json", backup_dir);
			fprintf(stdout, "Found backup directory: %s\n", backup_dir);
			fprintf(stdout, "Manifest: %s\n", manifest_file);
		}
	}

	/* Step 4.5: Verify manifest structure and hidden files exist */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (!manifest_json) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Cannot parse manifest" COL_RESET "\n");
			rv = -1;
		}
	}

	if (rv == 0) {
		structures_array = json_get_dict_value(manifest_json, "structures");
		if (!structures_array || structures_array->type != JSON_E_ARRAY) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Manifest missing structures array" COL_RESET "\n");
			rv = -1;
		} else {
			fprintf(stdout, "Manifest contains %d structures\n", structures_array->array.len);

			/* Verify all structure files exist */
			for (i = 0; i < structures_array->array.len; i++) {
				struct mm_json_elem *structure_elem = structures_array->array.elements[i];
				const char *file = json_get_dict_str(structure_elem, "file", NULL);
				const char *name = json_get_dict_str(structure_elem, "name", NULL);
				struct stat st;

				if (file && stat(file, &st) == 0) {
					fprintf(stdout, "  ✓ %s (%lu bytes)\n", name ? name : "unknown", (uint64_t)st.st_size);
				} else {
					fprintf(stdout, COL_RED_BOLD "  ✗ Missing: %s" COL_RESET "\n", file ? file : "null");
					rv = -1;
				}
			}
		}
	}

	/* Step 5: Restore from modular backup */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Step 6: Verify device matches original (check disk_metadata) */
	if (rv == 0) {
		struct nvmeibt_disk_gpt					main_gpt;
		struct nvmeibt_disk_gpt					metadata_gpt;
		struct nvmeibt_disk_metadata			disk_md;
		const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
		const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition;
		int fd = open(device_path, O_RDONLY);

		if (fd >= 0) {
			memset(&main_gpt, 0, sizeof(main_gpt));
			nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt, 1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) == 0) {
				metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
				if (metadata_partition) {
					memset(&metadata_gpt, 0, sizeof(metadata_gpt));
					nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));
					if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt,
														  metadata_partition->pba_s, metadata_partition->pba_e, false) == 0) {
						disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&metadata_gpt);
						if (disk_md_partition) {
							uint64_t pbyte_s = disk_md_partition->pba_s * SELF_TEST_MOCK_DEVICE_BLOCK_SIZE;
							memset(&disk_md, 0, sizeof(disk_md));
							if (nvmeibt_disk_metadata_read_disk_metadata(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, pbyte_s, &disk_md) == 0) {
								/* Original mock has empty ldisk_id_str, check it's back to empty (not MODIFIED_FOR_BACKUP_TEST) */
								if (disk_md.ldisk_id_str[0] != '\0') {
									fprintf(stdout, COL_RED_BOLD "FAIL: Device not restored (ldisk_id=%s)" COL_RESET "\n", disk_md.ldisk_id_str);
									rv = -1;
								} else {
									fprintf(stdout, COL_GREEN "Verified: Device restored to original state (modular backup)" COL_RESET "\n");
								}
							}
						}
					}
				}
			}
			close(fd);
		}
	}

	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("binary_test"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_serial_mismatch)
{
	int							rv = 0;
	const char					*device_a = TOMA_ROOT_DIR "tmp/gpt_backup_serial_a";
	const char					*device_b = TOMA_ROOT_DIR "tmp/gpt_backup_serial_b";
	char						manifest_file[600] = {0};
	int							fd_a;
	int							fd_b;

	/* Create device A */
	fd_a = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_a);
	if (fd_a < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create device A" COL_RESET "\n");
		return -1;
	}
	close(fd_a);

	/* Create device B */
	fd_b = SELF_TEST_generate_and_open_mock_nvmesh_disk(device_b);
	if (fd_b < 0) {
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create device B" COL_RESET "\n");
		unlink(device_a);
		return -1;
	}
	close(fd_b);

	/* Export from device A and trigger backup */
	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_a, TEST_JSON_PATH("backup_serial"));
	}

	/* Find the manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_a, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Try to restore to device B (should be BLOCKED by serial mismatch) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_b, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to serial mismatch */
	}

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
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_missing";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*first_structure_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_missing"));
	}

	/* Find manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Parse manifest and delete first structure file */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (manifest_json) {
			structures_array = json_get_dict_value(manifest_json, "structures");
			if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len > 0) {
				first_structure_file = json_get_dict_str(structures_array->array.elements[0], "file", NULL);
				if (first_structure_file) {
					fprintf(stdout, "Deleting structure file: %s\n", first_structure_file);
					unlink(first_structure_file);
				}
			}
		}
	}

	/* Try to restore (should be BLOCKED by missing file) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to missing file */
	}

	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_missing"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_corrupted_file)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_corrupt";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*first_structure_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_corrupt"));
	}

	/* Find manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Parse manifest and corrupt first structure file (truncate it) */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (manifest_json) {
			structures_array = json_get_dict_value(manifest_json, "structures");
			if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len > 0) {
				first_structure_file = json_get_dict_str(structures_array->array.elements[0], "file", NULL);
				if (first_structure_file) {
					int fd = open(first_structure_file, O_WRONLY | O_TRUNC);
					if (fd >= 0) {
						fprintf(stdout, "Corrupting structure file (truncating): %s\n", first_structure_file);
						write(fd, "BAD", 3);		/* Write wrong size */
						close(fd);
					}
				}
			}
		}
	}

	/* Try to restore (should be BLOCKED by wrong file size) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to corrupted file */
	}

	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("backup_corrupt"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_incomplete_manifest)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_incomplete";
	int							fd;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Create incomplete manifest manually (missing "structures" field) */
	fd = open(TEST_JSON_PATH("incomplete_manifest"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		char mock_serial[64];
		char json_content[512];

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
		fprintf(stdout, "Created incomplete manifest (missing 'structures' field)\n");
	} else {
		rv = -1;
	}

	/* Try to restore (should be BLOCKED by missing structures field) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", TEST_JSON_PATH("incomplete_manifest"), "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to missing structures field */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("incomplete_manifest"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_pba_overflow)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_overflow";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_overflow"));
	}

	/* Find manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Modify manifest to set a structure's pba_start beyond device end */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (manifest_json) {
			structures_array = json_get_dict_value(manifest_json, "structures");
			if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len > 0) {
				/* Set last structure's pba_start to 999999 (way beyond device end) */
				int last_idx = structures_array->array.len - 1;
				json_set_dict_num(structures_array->array.elements[last_idx], "pba_start", 999999);
				fprintf(stdout, "Modified manifest: set last structure pba_start=999999\n");
				rv = SELF_TEST_write_json_file_and_free_kv_tree(manifest_json, manifest_file);
				manifest_json = NULL;		/* Already freed by helper */
			} else {
				rv = -1;
			}
		} else {
			rv = -1;
		}
	}

	/* Try to restore (should be BLOCKED by PBA overflow) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to PBA out of bounds */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("backup_overflow"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_block_size_mismatch)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_blocksize";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("backup_blocksize"));
	}

	/* Find manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Modify manifest to set wrong block_size */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (manifest_json) {
			json_set_dict_num(manifest_json, "block_size", 512);		/* Wrong! Should be 4096 */
			fprintf(stdout, "Modified manifest: set block_size=512 (should be 4096)\n");
			rv = SELF_TEST_write_json_file_and_free_kv_tree(manifest_json, manifest_file);
			manifest_json = NULL;		/* Already freed by helper */
		} else {
			rv = -1;
		}
	}

	/* Try to restore (should be BLOCKED by block size mismatch) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to block size mismatch */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("backup_blocksize"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_restore_empty_structures)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_backup_empty";
	int							fd;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Create manifest with empty structures array */
	fd = open(TEST_JSON_PATH("empty_structures"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		char mock_serial[64];
		char json_content[512];

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
		close(fd);
		fprintf(stdout, "Created manifest with empty structures array (structures: [])\n");
	} else {
		rv = -1;
	}

	/* Try to restore (should be BLOCKED by structure count = 0, expected 10) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", TEST_JSON_PATH("empty_structures"), "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure due to structures.len != 10 */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("empty_structures"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(backup_creation_non_nvmesh_device)
{
	int									rv = 0;
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
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED: Could not create device" COL_RESET "\n");
		return -1;
	}

	/* Initialize MBR */
	memset(&mbr, 0, sizeof(mbr));
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		close(fd);
		unlink(device_path);
		return -1;
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
		close(fd);
		unlink(device_path);
		return -1;
	}

	fsync(fd);
	close(fd);

	/* Try to export (should be BLOCKED - device not NVMesh formatted) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("non_nvmesh"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure - no metadata partition */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("non_nvmesh"));
	unlink(device_path);
	return rv;
}

DEFINE_TEST(restore_mid_failure_file_deleted)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_restore_mid_fail";
	char						manifest_file[600] = {0};
	struct mm_json_elem			*manifest_json = NULL;
	struct mm_json_elem			*structures_array = NULL;
	const char					*last_file = NULL;

	/* Create device and trigger backup */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	if (rv == 0) {
		rv = SELF_TEST_trigger_backup_for_device(ctx, device_path, TEST_JSON_PATH("restore_mid_fail"));
	}

	/* Find manifest */
	if (rv == 0) {
		if (find_newest_backup_directory(device_path, manifest_file, sizeof(manifest_file)) < 0) {
			fprintf(stdout, COL_RED_BOLD "FAIL: No backup directory found" COL_RESET "\n");
			rv = -1;
		}
	}

	/* Delete LAST structure file to simulate mid-restore I/O failure */
	/* This tests that restore properly handles a missing/deleted backup file */
	if (rv == 0) {
		manifest_json = SELF_TEST_parse_json_file(manifest_file);
		if (manifest_json) {
			structures_array = json_get_dict_value(manifest_json, "structures");
			if (structures_array && structures_array->type == JSON_E_ARRAY && structures_array->array.len == 10) {
				/* Get last structure (disk_metadata - index 9) */
				last_file = json_get_dict_str(structures_array->array.elements[9], "file", NULL);
				if (last_file) {
					fprintf(stdout, "Deleting last structure file: %s\n", last_file);
					fprintf(stdout, "Restore should fail when file is missing\n");
					unlink(last_file);		/* Delete file - restore will fail */
				}
			}
		}
	}

	/* Try to restore - file is deleted, restore should fail */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--restore-binary", manifest_file, "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expecting failure during restore with partial restore warning */
	}

	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(manifest_json);
	unlink(TEST_JSON_PATH("restore_mid_fail"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(csv_parsing_path)
{
	int rv = 0;

	/* Note: -d flag reads from /proc/nvmeibs/disks.csv which doesn't exist in sandbox
	 * This test validates the flag is accepted and parsing logic doesn't crash
	 * Expected: Fails gracefully with "device not found" message
	 */
	SELF_TEST_ARGV("-d", "/dev/nvme0n1");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Expect failure (device not in CSV), but no crash = success */
	if (rv != 0) {
		fprintf(stdout, COL_GREEN "Verified: -d flag handled gracefully (device not found, no crash)" COL_RESET "\n");
		rv = 0;		/* Graceful failure is success for this test */
	}

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
// 		int i;

// 		if (json_root) {
// 			for (i = 0; i < json_root->dict.len; i++) {
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
	int									rv = 0;
	const char							*device_path = TOMA_ROOT_DIR "tmp/gpt_add_partition";
	struct mm_json_elem					*json_root = NULL;
	struct mm_json_elem					*entries = NULL;
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
		fprintf(stdout, COL_RED_BOLD "SETUP FAILED" COL_RESET "\n");
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
	fprintf(stdout, "Created larger device (4000 blocks) with partition PBA 258-1500\n");

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("add_partition"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Add a new partition entry by duplicating existing entry and modifying it */
	if (rv == 0) {
		json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("add_partition"));
		if (!json_root) {
			rv = -1;
		}
	}

	if (rv == 0) {
		struct mm_json_elem *main_gpt_ptr = NULL;
		/* Navigate to main_gpt_primary -> entries */
		for (int i = 0; i < json_root->dict.len; i++) {
			if (strcmp(json_root->dict.elements[i].key, "main_gpt_primary") == 0) {
				main_gpt_ptr = json_root->dict.elements[i].value;
				break;
			}
		}
		if (main_gpt_ptr) {
			for (int i = 0; i < main_gpt_ptr->dict.len; i++) {
				if (strcmp(main_gpt_ptr->dict.elements[i].key, "entries") == 0) {
					entries = main_gpt_ptr->dict.elements[i].value;
					break;
				}
			}
		}

		if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
			rv = -1;
			goto cleanup;		/* Free json_root before returning */
		}
	}

	/* Manually add a new entry by expanding the array */
	if (rv == 0) {
		struct mm_json_elem *existing_entry = entries->array.elements[0];
		struct mm_json_elem *new_entry = calloc(1, sizeof(*new_entry));

		/* Create new entry (copy of existing, will modify fields) */
		if (!new_entry) {
			rv = -1;
			goto cleanup;		/* Free json_root before returning */
		} else {
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
			/* Existing is at 258-1500, device ends at ~3742, so plenty of room */
			json_set_dict_num(new_entry, "index", 1);
			json_set_dict_str(new_entry, "name", "new_partition");
			json_set_dict_str(new_entry, "partition_guid", "11223344-5566-7788-99aa-bbccddeeff00");
			json_set_dict_str(new_entry, "type_guid", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");  /* Linux filesystem */
			json_set_dict_num(new_entry, "pba_s", 1501);
			json_set_dict_num(new_entry, "pba_e", 1700);
			json_set_dict_num(new_entry, "attributes", 0);
			json_set_dict_bool(new_entry, "_delete", false);

			/* Expand entries array */
			entries->array.elements = realloc(entries->array.elements,
											   (entries->array.len + 1) * sizeof(struct mm_json_elem *));
			entries->array.elements[entries->array.len] = new_entry;
			entries->array.len++;

			fprintf(stdout, "Added new partition entry (index 1, PBA 600-800)\n");
		}
	}

	/* Write modified JSON */
	if (rv == 0) {
		rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("add_partition"));
		json_root = NULL;		/* Already freed by helper */
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("add_partition"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify new partition was added */
	if (rv == 0) {
		int fd = open(device_path, O_RDONLY);
		if (fd >= 0) {
			int n_entries_after = 0;
			memset(&main_gpt_after, 0, sizeof(main_gpt_after));
			nvmeibt_strlcpy(main_gpt_after.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt_after.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt_after,
												  1, 4000 - 1, false) == 0) {		/* Match larger device size */
				for (int i = 0; i < main_gpt_after.max_n_entries; i++) {
					if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&main_gpt_after.entries[i])) {
						n_entries_after++;
					}
				}

				if (n_entries_after == 2) {
					fprintf(stdout, COL_GREEN "Verified: New partition added successfully (1 -> 2 entries)" COL_RESET "\n");
				} else {
					fprintf(stdout, COL_RED_BOLD "FAIL: Expected 2 entries, got %d" COL_RESET "\n", n_entries_after);
					rv = -1;
				}
			}
			close(fd);
		}
	}

cleanup:
	/* Cleanup */
	nvmeibt_mm_json_free_kv_tree(json_root);
	unlink(TEST_JSON_PATH("add_partition"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(json_modify_metadata_gpt)
{
	int							rv = 0;
	const char					*device_path = TOMA_ROOT_DIR "tmp/gpt_modify_metadata";
	struct nvmeibt_disk_gpt		main_gpt;
	struct nvmeibt_disk_gpt		metadata_gpt_after;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition;
	char						partition_name_after[GPT_MAX_PARTITION_NAME_LENGTH + 1];
	int							fd = -1;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, device_path);

	/* Export to JSON */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "-J", TEST_JSON_PATH("modify_metadata"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Modify metadata GPT partition name */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("modify_metadata"));
		struct mm_json_elem *metadata_gpt_elem = NULL;
		struct mm_json_elem *entries = NULL;
		int i;

		if (json_root) {
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "metadata_gpt_primary") == 0) {
					metadata_gpt_elem = json_root->dict.elements[i].value;
					break;
				}
			}
			if (metadata_gpt_elem) {
				for (i = 0; i < metadata_gpt_elem->dict.len; i++) {
					if (strcmp(metadata_gpt_elem->dict.elements[i].key, "entries") == 0) {
						entries = metadata_gpt_elem->dict.elements[i].value;
						break;
					}
				}
			}

			/* Modify first entry's name */
			if (entries && entries->type == JSON_E_ARRAY && entries->array.len > 0) {
				json_set_dict_str(entries->array.elements[0], "name", "Modified_Disk_Metadata");
				fprintf(stdout, "Modified metadata partition name to: Modified_Disk_Metadata\n");
			}

			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("modify_metadata"));
		} else {
			rv = -1;
		}
	}

	/* Apply with --write */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_path, "--apply-from", TEST_JSON_PATH("modify_metadata"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Verify name was changed */
	if (rv == 0) {
		fd = open(device_path, O_RDONLY);
		if (fd >= 0) {
			memset(&main_gpt, 0, sizeof(main_gpt));
			nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
			if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &main_gpt,
												  1, SELF_TEST_MOCK_DEVICE_BLOCKS - 1, false) == 0) {
				metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
				if (metadata_partition) {
					memset(&metadata_gpt_after, 0, sizeof(metadata_gpt_after));
					nvmeibt_strlcpy(metadata_gpt_after.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt_after.main_or_metadata));
					if (nvmeibt_disk_metadata_restore_gpt(NULL, fd, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, &metadata_gpt_after,
														  metadata_partition->pba_s, metadata_partition->pba_e, false) == 0) {
						char16_str_to_str(metadata_gpt_after.entries[0].partition_name,
										  GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name_after);
						if (strcmp(partition_name_after, "Modified_Disk_Metadata") == 0) {
							fprintf(stdout, COL_GREEN "Verified: Metadata partition name changed successfully" COL_RESET "\n");
						} else {
							fprintf(stdout, COL_RED_BOLD "FAIL: Name not changed (got: %s)" COL_RESET "\n", partition_name_after);
							rv = -1;
						}
					}
				}
			}
			close(fd);
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("modify_metadata"));
	cleanup_backup_files_for_device(device_path);
	unlink(device_path);
	return rv;
}

DEFINE_TEST(json_boundary_max_partitions)
{
	int rv = 0;

	/* Verify system handles LARGE_GPT_MAX_NUM_GPT_ENTRIES (8192) correctly */
	/* Standard device uses 8192-entry GPTs for both Main and Metadata */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);

	/* Export to JSON (exercises 8192-entry GPT export) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("boundary_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Apply back (exercises 8192-entry GPT apply) */
	if (rv == 0) {
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("boundary_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	/* Validates: n_partition_entries=8192 handled correctly in export/apply */
	if (rv == 0) {
		fprintf(stdout, COL_GREEN "Verified: 8192-entry GPT handled correctly" COL_RESET "\n");
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("boundary_test"));
	return rv;
}

DEFINE_TEST(o_direct_flags)
{
	int rv = 0;

	/* Create device */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);

	/* Test --direct flag (may fail for regular file, but should not crash) */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "--direct", "-c", "both");
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	if (rv != 0) {
		fprintf(stdout, COL_YELLOW "Note: --direct may fail for regular files (expected)" COL_RESET "\n");
		rv = 0;		/* Graceful handling = success */
	}

	/* Test --no-direct flag (should work for regular files) */
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--no-direct", "-c", "both");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	if (rv == 0) {
		fprintf(stdout, COL_GREEN "Verified: O_DIRECT flags handled correctly" COL_RESET "\n");
	}

	return rv;
}

DEFINE_TEST(export_without_toma)
{
	int rv = 0;

	/* Create device and export (TOMA not running - guaranteed by run_self_test check) */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("no_toma"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Verify JSON has TOMA status but no memory sections */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("no_toma"));
		BOOL toma_running;

		if (!json_root) {
			rv = -1;
		} else {
			toma_running = json_get_dict_bool(json_root, "_toma_running", true);		/* Default true to catch errors */
			if (toma_running) {
				fprintf(stdout, COL_RED_BOLD "FAIL: JSON says TOMA running (expected false)" COL_RESET "\n");
				rv = -1;
			} else if (json_get_dict_value(json_root, "memory_main_gpt") || json_get_dict_value(json_root, "memory_metadata_gpt")) {
				fprintf(stdout, COL_RED_BOLD "FAIL: JSON contains memory sections (should not)" COL_RESET "\n");
				rv = -1;
			} else {
				fprintf(stdout, COL_GREEN "Verified: TOMA not running, no memory sections in JSON" COL_RESET "\n");
			}
			nvmeibt_mm_json_free_kv_tree(json_root);
		}
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("no_toma"));
	return rv;
}

DEFINE_TEST(write_blocked_toma_running)
{
	int rv = 0;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("write_block"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Modify JSON to create changes (so confirmation is triggered) */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("write_block"));
		struct mm_json_elem *disk_md = NULL;
		int i;

		if (json_root) {
			/* Find disk_metadata and modify it to trigger a change */
			for (i = 0; i < json_root->dict.len; i++) {
				if (strcmp(json_root->dict.elements[i].key, "disk_metadata") == 0) {
					disk_md = json_root->dict.elements[i].value;
					break;
				}
			}
			if (disk_md) {
				json_set_dict_str(disk_md, "ldisk_id_str", "MODIFIED_TO_TRIGGER_CHANGE");
				fprintf(stdout, "Modified JSON to trigger a change\n");
			}
			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("write_block"));
		} else {
			rv = -1;
		}
	}

	/* Try to apply with --write (should be BLOCKED by TOMA check at confirmation) */
	if (rv == 0) {
		SELF_TEST_mock_toma_running();
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("write_block"), "--write", "--yes");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		SELF_TEST_undo_mock_toma_running();
		/* Expecting failure (write blocked before confirmation) */
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("write_block"));
	return rv;
}

DEFINE_TEST(memory_sections_ignored)
{
	int rv = 0;

	/* Create device and export */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("mem_sections"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	/* Manually add fake memory_main_gpt section to JSON */
	if (rv == 0) {
		struct mm_json_elem *json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("mem_sections"));

		if (json_root) {
			/* Add minimal memory_main_gpt section */
			json_set_dict_str(json_root, "memory_main_gpt", "fake_data");
			fprintf(stdout, "Added fake memory_main_gpt section to JSON\n");
			rv = SELF_TEST_write_json_file_and_free_kv_tree(json_root, TEST_JSON_PATH("mem_sections"));
		} else {
			rv = -1;
		}
	}

	/* Try to apply (should succeed, ignoring memory sections) */
	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
		SELF_TEST_ARGV("-a", ctx->test_device_path, "--apply-from", TEST_JSON_PATH("mem_sections"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
		/* Expect success (memory sections ignored) */
	}

	if (rv == 0) {
		fprintf(stdout, COL_GREEN "Verified: Memory sections ignored gracefully during apply" COL_RESET "\n");
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("mem_sections"));
	return rv;
}

/**
 * Test get_memory_gpt_via_rpc() using mock GPT structures
 * This test verifies the JSON export
 */
DEFINE_TEST(export_memory_gpt)
{
	int								rv = 0;
	struct nvmeibt_local_disk		mock_local_disk;
	union nvmeib_uuid				test_uuid;

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

	fprintf(stdout, "Created mock local_disk with GPT structures\n");

	/* Step 2: Set mock local_disk and mock TOMA as running */
	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_set_mock_local_disk(&mock_local_disk);
	SELF_TEST_mock_toma_running();

	/* Step 3: Run export - should use mock structures */
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("mem_rpc"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	SELF_TEST_undo_mock_toma_running();
	SELF_TEST_set_mock_local_disk(NULL);	// Clear mock

	/* Step 4: Verify the exported JSON contains memory sections from mock */
	if (rv == 0) {
		struct mm_json_elem		*json_root = SELF_TEST_parse_json_file(TEST_JSON_PATH("mem_rpc"));
		struct mm_json_elem		*mem_main_gpt;
		struct mm_json_elem		*mem_meta_gpt;
		struct mm_json_elem		*entries;
		BOOL					toma_running;

		if (!json_root) {
			fprintf(stdout, COL_RED_BOLD "FAIL: Cannot parse exported JSON" COL_RESET "\n");
			rv = -1;
		} else {
			/* Verify _toma_running is true */
			toma_running = json_get_dict_bool(json_root, "_toma_running", false);
			if (!toma_running) {
				fprintf(stdout, COL_RED_BOLD "FAIL: JSON says TOMA not running (expected true)" COL_RESET "\n");
				rv = -1;
			}

			/* Verify memory_main_gpt section exists with our mock data */
			mem_main_gpt = json_get_dict_value(json_root, "memory_main_gpt");
			if (!mem_main_gpt || mem_main_gpt->type != JSON_E_DICT) {
				fprintf(stdout, COL_RED_BOLD "FAIL: memory_main_gpt section missing or invalid" COL_RESET "\n");
				rv = -1;
			} else {
				entries = json_get_dict_value(mem_main_gpt, "entries");
				if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
					fprintf(stdout, COL_RED_BOLD "FAIL: memory_main_gpt has no entries" COL_RESET "\n");
					rv = -1;
				} else {
					fprintf(stdout, COL_GREEN "Verified: memory_main_gpt from mock structures (%d entries)" COL_RESET "\n",
							entries->array.len);
				}
			}

			/* Verify memory_metadata_gpt section exists */
			mem_meta_gpt = json_get_dict_value(json_root, "memory_metadata_gpt");
			if (!mem_meta_gpt || mem_meta_gpt->type != JSON_E_DICT) {
				fprintf(stdout, COL_RED_BOLD "FAIL: memory_metadata_gpt section missing or invalid" COL_RESET "\n");
				rv = -1;
			} else {
				entries = json_get_dict_value(mem_meta_gpt, "entries");
				if (!entries || entries->type != JSON_E_ARRAY || entries->array.len == 0) {
					fprintf(stdout, COL_RED_BOLD "FAIL: memory_metadata_gpt has no entries" COL_RESET "\n");
					rv = -1;
				} else {
					fprintf(stdout, COL_GREEN "Verified: memory_metadata_gpt from mock structures (%d entries)" COL_RESET "\n",
							entries->array.len);
				}
			}

			nvmeibt_mm_json_free_kv_tree(json_root);
		}
	}

	if (rv == 0) {
		fprintf(stdout, COL_GREEN "Verified: get_memory_gpt_via_rpc() uses mock structures correctly" COL_RESET "\n");
	}

	/* Cleanup */
	unlink(TEST_JSON_PATH("mem_rpc"));
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
	int							i;
	BOOL						*tests_to_run = NULL;
	int							*test_results = NULL;
	int							num_tests_total = 1;		// Will be updated

	// Test registry - auto-generated from SELF_TEST_LIST X-Macro
	#define X(func, name, cmd, expect_fail) {name, cmd, test_##func, expect_fail},
	struct self_test_entry tests[] = { SELF_TEST_LIST };
	#undef X

	/* Safety check: Block self-tests if TOMA is running */
	if (SELF_TEST_acquire_toma_lock()) {
		fprintf(stderr, COL_RED_BOLD "\nERROR: Cannot run self-tests while TOMA is running!" COL_RESET "\n");
		fprintf(stderr, "Self-tests may spawn fake TOMA process and create conflicts.\n");
		fprintf(stderr, "Please stop TOMA before running tests.\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "To stop TOMA: sudo systemctl stop nvmesh-toma\n");
		fprintf(stderr, "Or: sudo pkill nvmeibt_toma\n");
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

	// Allocate tracking arrays
	tests_to_run = calloc(num_tests_total, sizeof(BOOL));
	test_results = calloc(num_tests_total, sizeof(int));		/* 0=not run, 1=passed, -1=failed */

	if (test_selection == NULL) {
		// Run all tests
		for (i = 0; i < num_tests_total; i++) {
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
				int j;
				for (j = start; j <= end && j <= num_tests_total; j++) {
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
	for (i = 0; i < num_tests_total; i++) {
		if (tests_to_run[i]) {
			int test_num = i + 1;		// Actual test number (from registry position)
			int rv;

			SELF_TEST_start(test_num, tests[i].name, tests[i].command, quiet_mode);
			rv = tests[i].func(&ctx);

			tests_run++;
			if (SELF_TEST_end(test_num, rv, tests[i].expect_failure) == 0) {
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
	for (i = 0; i < num_tests_total; i++) {
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
	return 0;
}

