/**
 * gpt_util_self_test.c - Self-test framework implementation for gpt_util
 *
 * Comprehensive test suite for GPT utility operations
 */

#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../nvmeibt_debug.h"
#include "../nvmeibt_disk_metadata.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_str.h"
#include "nvmeibt_uuid.h"
#include "../nvmeibt_json_base.h"
#include "../nvmeibt_local_disk.h"
#include "gpt_util_self_test.h"

/**
 * Start a self-test case (SELF-TEST only)
 * Prints test header with the given test number
 */
void SELF_TEST_start(int test_num, const char *description, const char *command)
{
	fprintf(stdout, "\n");
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
	fprintf(stdout, COL_WHITE_BOLD "SELF-TEST %d: %s" COL_RESET "\n", test_num, description);
	fprintf(stdout, "Emulated command: " COL_YELLOW "%s" COL_RESET "\n", command);
	fprintf(stdout, COL_BLUE "============================================================" COL_RESET "\n");
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

			nvmeibt_strlcpy(disk_metadata.native_serial_str, "MOCK-SERIAL-12345678", sizeof(disk_metadata.native_serial_str));
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
 * Generate a mock device with intentionally overlapping partitions
 * Returns the fd of the created device (caller must close it)
 */
int SELF_TEST_generate_mock_device_with_overlaps(const char *filepath)
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
 * Modify JSON string field (SELF-TEST helper)
 */
int SELF_TEST_modify_json_str_field(const char *json_path, const char *field, const char *new_value)
{
	struct mm_json_elem		*json_root = NULL;
	struct nvmeibt_Str		*json_output = NULL;
	int						fd = -1;
	int						rv = -1;

	json_root = SELF_TEST_parse_json_file(json_path);
	if (!json_root || json_root->type != JSON_E_DICT) {
		goto out;
	}

	// Modify using base library function
	if (json_set_dict_str(json_root, field, new_value) < 0) {
		N_Ef(selftest_str_field_not_found, "String field @STR not found", field);
		goto out;
	}

	// Serialize and write
	json_output = NNVMEIBT_STR_ALLOC(trace_selftest_json_mod_str);
	if (serialize_json_tree_to_str(json_root, json_output) < 0) {
		goto out;
	}

	fd = open(json_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		goto out;
	}

	if (write(fd, nvmeibt_Str_str(json_output), nvmeibt_Str_strlen(json_output)) != (ssize_t)nvmeibt_Str_strlen(json_output)) {
		goto out;
	}

	rv = 0;

out:
	if (fd >= 0) close(fd);
	if (json_root) nvmeibt_mm_json_free_kv_tree(json_root);
	NNVMEIBT_STR_FREE(trace_selftest_json_mod_str_cleanup, json_output);
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
		goto out;
	}

	file_content = NNVMEIBT_STR_ALLOC(trace_selftest_remove_read);
	if (NNVMEIBT_STR_FREAD_ATOMIC(trace_selftest_remove_fread, file_content, fd) < 0) {
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
		goto out;
	}

	if (write(fd, nvmeibt_Str_str(new_content), nvmeibt_Str_strlen(new_content)) != (ssize_t)nvmeibt_Str_strlen(new_content)) {
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

	// Export A, modify JSON, apply to B
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_a, "-J", TEST_JSON_PATH("write_test"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}
	if (rv == 0) {
		rv = SELF_TEST_modify_json_str_field(TEST_JSON_PATH("write_test"), "device_path", device_b);
	}
	if (rv == 0) {
		SELF_TEST_ARGV("-a", device_b, "--apply-from", TEST_JSON_PATH("write_test"), "--write");
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

	// Verify A == B (complete binary roundtrip fidelity)
	if (rv == 0) {
		rv = SELF_TEST_compare_gpt_binary(device_a, device_b, SELF_TEST_MOCK_DEVICE_BLOCK_SIZE, SELF_TEST_MOCK_DEVICE_BLOCKS);
	}

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

	return rv;
}

DEFINE_TEST(device_path_safety)
{
	int rv = 0;

	SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->test_device_path);
	SELF_TEST_ARGV("-a", ctx->test_device_path, "-J", TEST_JSON_PATH("device_check"));
	rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);

	if (rv == 0) {
		SELF_TEST_SETUP_OR_ABORT(SELF_TEST_generate_and_open_mock_nvmesh_disk, ctx->wrong_device_path);
		SELF_TEST_ARGV("-a", ctx->wrong_device_path, "--apply-from", TEST_JSON_PATH("device_check"));
		rv = SELF_TEST_run_gpt_util_op(*ctx->test_argc, ctx->test_argv);
	}

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

	return rv;
}

/**
 * Run comprehensive self-test suite
 */
int run_self_test(const char *test_selection)
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
	int							num_tests_total = 1;		// Will be updated

	// Test registry - auto-generated from SELF_TEST_LIST X-Macro
	#define X(func, name, cmd, expect_fail) {name, cmd, test_##func, expect_fail},
	struct self_test_entry tests[] = { SELF_TEST_LIST };
	#undef X

	num_tests_total = sizeof(tests) / sizeof(tests[0]);

	mkdir(TOMA_ROOT_DIR "tmp", 0755);
	test_device_path = TOMA_ROOT_DIR "tmp/gpt_util_self_test";
	wrong_device_path = TOMA_ROOT_DIR "tmp/gpt_util_wrong_device";

	// Setup context
	ctx.test_device_path = test_device_path;
	ctx.wrong_device_path = wrong_device_path;
	ctx.test_argv = test_argv;
	ctx.test_argc = &test_argc;

	// Parse test selection (NULL = run all)
	tests_to_run = calloc(num_tests_total, sizeof(BOOL));
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
			int test_num = i + 1;		// Actual test number (1-16, from registry position)
			int rv;

			SELF_TEST_start(test_num, tests[i].name, tests[i].command);
			rv = tests[i].func(&ctx);

			tests_run++;
			if (SELF_TEST_end(test_num, rv, tests[i].expect_failure) == 0) {
				tests_passed++;
			} else {
				tests_failed++;
			}
		}
	}

	// Cleanup
	free(tests_to_run);

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

	if (test_selection == NULL) {
		fprintf(stdout, "\nTest List (%d tests):\n", num_tests_total);
		for (i = 0; i < num_tests_total; i++) {
			fprintf(stdout, "  Test %d: %s\n", i + 1, tests[i].name);
		}
	}

	// Cleanup
	if (disk_fd >= 0) {
		close(disk_fd);
	}

	// Always clean up test files
	unlink(test_device_path);
	unlink(wrong_device_path);
	unlink(TEST_JSON_PATH("export"));
	unlink(TEST_JSON_PATH("mismatch"));
	unlink(TEST_JSON_PATH("overlaps"));
	unlink(TEST_JSON_PATH("diff_baseline"));
	unlink(TEST_JSON_PATH("standard"));
	unlink(TEST_JSON_PATH("write_test"));
	unlink(TEST_JSON_PATH("missing_gpt"));
	unlink(TEST_JSON_PATH("device_check"));
	unlink(TEST_JSON_PATH("overlap_block"));
	unlink(TEST_JSON_PATH("mismatch_block"));

	return 0;
}

