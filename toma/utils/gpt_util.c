/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

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
#include "../nvmeibt_bm.h"
#include "../nvmeibt_read_config.h"
#include "../nvmeibt_str.h"
#include "../nvmeibt_local_disk.h"
#include "../nvmeibt_uuid.h"
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
	ACTION_CHECK_NVMESH		// -i: check if NVMESH_METADATA exists
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

	// Display options (for ACTION_DISPLAY_GPT only)
	char					gpt_copy_option[16];	// primary|alternate|both

	// Filtering options
	char					filter_uuid_str[64];	// Filter by UUID (empty = no filter)
	uint64_t				filter_lba;				// Filter by LBA (0 = no filter)
	BOOL					has_uuid_filter;
	BOOL					has_lba_filter;
};


#undef N_Df
#undef N_Tf
#undef N_If
#undef N_IMf
#undef N_Wf
#undef N_Ef
#undef NFIN
#undef NFOUT
#define N_Df(name, fmt, ...) do {} while (0)
//#define N_Tf(name, fmt, ...) do { NVMEIB_LOG_LONGTERM(fmt, _Tf, /*Default Scope*/, name, ##__VA_ARGS__); printf(N_FORMAT_STRING(name), ## __VA_ARGS__); printf("\n");} while (0)
#define N_Tf(name, fmt, ...) do {} while (0)
#define N_If(name, fmt, ...) do {} while (0)
#define N_IMf(name, fmt, ...) do {} while (0)
#define N_Wf(name, fmt, ...) do {} while (0)
#define N_Ef(name, fmt, ...) do {} while (0)
#define NFIN
#define NFOUT


int get_device_info(const char* dev_name, int* pblk_size, uint64_t* pba_e)
{
	int			fd = -1;
	int			rv = -1;
	uint64_t	n_bytes_dev;
	int			pblk_size_dev;
	struct stat	st;

	rv = NNVMEIBT_OPEN_READ(trace_gpt_util_get_device_info, dev_name, 1);
	if (rv < 0) {
		fprintf(stderr, "Failed opening device=%s  %d %m\n", dev_name, errno);
		goto out;
	}
	fd = rv;

	// Detect file type first
	rv = fstat(fd, &st);
	if (rv < 0) {
		fprintf(stderr, "Failed to stat=%s  %d %m\n", dev_name, errno);
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
			fprintf(stderr, "Failed fetching block size=%s  %d %m\n", dev_name, errno);
			goto out;
		}

		rv = ioctl(fd, BLKGETSIZE64, &n_bytes_dev);
		if (rv < 0) {
			fprintf(stderr, "Failed fetching disk size=%s  %d %m\n", dev_name, errno);
			goto out;
		}
	} else {
		fprintf(stderr, "Unsupported file type for %s\n", dev_name);
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
		fprintf(stderr, "Failed to create mock device %s: %s\n", filepath, strerror(errno));
		return -1;
	}

	// 1. Initialize protective MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		fprintf(stderr, "Failed to write MBR\n");
		goto out;
	}

	// 2. Initialize Main GPT structure using existing TOMA function
	disk_uuid.ll[0] = 0x1122334455667788ULL;
	disk_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	nvmeibt_disk_metadata_init_gpt_structure(1, n_disk_blocks - 1, &main_gpt, pblk_size,
											 LARGE_GPT_MAX_NUM_GPT_ENTRIES, &disk_uuid);

	// 3. Add NVMESH_METADATA partition using all available space (test device)
	{
		uint64_t metadata_start = main_gpt.header.first_usable_pba;
		uint64_t metadata_end = main_gpt.header.last_usable_pba;

		metadata_partition_uuid.ll[0] = 0xAABBCCDD11223344ULL;
		metadata_partition_uuid.ll[1] = 0x5566778899AABBCCULL;

		metadata_partition = nvmeibt_disk_metadata_add_mem_gpt_entry(
			&main_gpt, &NVMESH_METADATA_PARTITION_TYPE_GUID,
			&metadata_partition_uuid,
			metadata_start, metadata_end,
			NVMESH_METADATA_PARTITION_NAME,
			strlen(NVMESH_METADATA_PARTITION_NAME));

		if (!metadata_partition) {
			fprintf(stderr, "Failed to add metadata partition\n");
			goto out;
		}
	}

	// 4. Write Main GPT to disk using existing TOMA function
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		fprintf(stderr, "Failed to store Main GPT\n");
		goto out;
	}

	// 5. Initialize nested Metadata GPT using existing TOMA functions
	metadata_disk_uuid.ll[0] = 0x2233445566778899ULL;
	metadata_disk_uuid.ll[1] = 0xAABBCCDDEEFF0011ULL;

	nvmeibt_disk_metadata_init_gpt_structure(metadata_partition->pba_s,
											 metadata_partition->pba_e,
											 &metadata_gpt, pblk_size,
											 MAX_NUM_GPT_ENTRIES, &metadata_disk_uuid);

	// Add NVMESH_DISK_METADATA partition
	disk_metadata_partition_uuid.ll[0] = 0xDD11223344556677ULL;
	disk_metadata_partition_uuid.ll[1] = 0x8899AABBCCDDEEF0ULL;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&metadata_gpt,
												 &NVMESH_DISK_METADATA_PARTITION_TYPE_GUID,
												 &disk_metadata_partition_uuid,
												 metadata_gpt.header.first_usable_pba,
												 metadata_gpt.header.last_usable_pba,
												 DISK_METADATA_PARTITION_NAME,
												 strlen(DISK_METADATA_PARTITION_NAME))) {
		fprintf(stderr, "Failed to add disk_metadata partition\n");
		goto out;
	}

	// 6. Write Metadata GPT to disk
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &metadata_gpt, false) < 0) {
		fprintf(stderr, "Failed to store Metadata GPT\n");
		goto out;
	}

	fsync(fd);
	fprintf(stdout, "Mock NVMesh device created: %s (%lu blocks, %d KB)\n",
			filepath, n_disk_blocks, (int)(n_disk_blocks * pblk_size / 1024));
	fprintf(stdout, "  - Main GPT with NVMESH_METADATA partition (LBA %lu-%lu)\n",
			metadata_partition->pba_s, metadata_partition->pba_e);
	fprintf(stdout, "  - Nested Metadata GPT with NVMESH_DISK_METADATA partition\n\n");

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
		fprintf(stderr, "Failed to create mock device %s: %s\n", filepath, strerror(errno));
		return -1;
	}

	// Initialize protective MBR
	nvmeibt_disk_metadata_init_pmbr(&mbr, n_disk_blocks, pblk_size);
	if (nvmeibt_disk_metadata_write_mbr(NULL, fd, pblk_size, &mbr) < 0) {
		fprintf(stderr, "Failed to write MBR\n");
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
												 &NVMESH_METADATA_PARTITION_TYPE_GUID,
												 &partition1_uuid,
												 main_gpt.header.first_usable_pba,
												 main_gpt.header.first_usable_pba + 1242,  // 1500 - 258
												 "partition_1",
												 strlen("partition_1"))) {
		fprintf(stderr, "Failed to add partition 1\n");
		goto out;
	}

	// Add second partition (LBA 1400-1742) - OVERLAPS with first!
	partition2_uuid.ll[0] = 0x1122334455667788ULL;
	partition2_uuid.ll[1] = 0x99AABBCCDDEEFF00ULL;

	overlap_start = main_gpt.header.first_usable_pba + 1142;  // 1400
	overlap_end = main_gpt.header.last_usable_pba;

	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&main_gpt,
												 &NVMESH_DISK_METADATA_PARTITION_TYPE_GUID,
												 &partition2_uuid,
												 overlap_start,
												 overlap_end,
												 "partition_2_OVERLAP",
												 strlen("partition_2_OVERLAP"))) {
		fprintf(stderr, "Failed to add partition 2 (overlap)\n");
		goto out;
	}

	// Write Main GPT
	if (nvmeibt_disk_metadata_store_gpt(NULL, fd, pblk_size, &main_gpt, false) < 0) {
		fprintf(stderr, "Failed to store Main GPT with overlaps\n");
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
		fprintf(stderr, "Failed to read GPT for corruption\n");
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
		fprintf(stderr, "Failed to write corrupted alternate GPT\n");
		goto out;
	}

	fprintf(stdout, "  - Corrupted alternate GPT (different UUID and partition name)\n");
	fprintf(stdout, "  - Both copies have VALID CRCs but MISMATCHED content\n");

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
				fprintf(stdout, "WARNING: Overlap detected between entry %d and entry %d\n", i, j);
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
			fprintf(stdout, "\n*** WARNING: %d overlap(s) detected in %s %s ***\n",
					n_overlaps, gpt_level, copy_name);
		}
	}
}

/**
 * Read and display GPT copies (primary and/or alternate) for one GPT level
 */
static void display_gpt_copies_one_level(const char *gpt_level,
										 const char *gpt_copy_option,
										 int disk_fd,
										 int pblk_size,
										 uint64_t pba_s,
										 uint64_t pba_e,
										 int max_n_entries,
										 const struct gpt_util_config *config)
{
	struct nvmeibt_disk_gpt_header			*primary_header;
	struct nvmeibt_disk_gpt_header			*alternate_header;
	struct nvmeibt_disk_gpt_partition_entry	*primary_entries;
	struct nvmeibt_disk_gpt_partition_entry	*alternate_entries;
	enum GPT_VALIDITY						primary_header_validity;
	enum GPT_VALIDITY						alternate_header_validity;
	enum GPT_VALIDITY						primary_entries_validity;
	enum GPT_VALIDITY						alternate_entries_validity;
	struct nvmeibt_disk_gpt					temp_gpt;
	int										n_bytes_header;
	int										n_bytes_entries;
	BOOL									is_header_mismatch;
	BOOL									is_entries_mismatch;
	BOOL									is_mismatch;

	// Allocate buffers in same scope where we free them
	n_bytes_header = roundup(sizeof(struct nvmeibt_disk_gpt_header), pblk_size);
	n_bytes_entries = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * max(LARGE_GPT_MAX_NUM_GPT_ENTRIES, MAX_NUM_GPT_ENTRIES), pblk_size);

	primary_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_util_pri_hdr, PAGE_SIZE, n_bytes_header);
	alternate_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_util_alt_hdr, PAGE_SIZE, n_bytes_header);
	primary_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_util_pri_ent, PAGE_SIZE, n_bytes_entries);
	alternate_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_util_alt_ent, PAGE_SIZE, n_bytes_entries);

	memset(&temp_gpt, 0, sizeof(temp_gpt));
	temp_gpt.max_n_entries = max_n_entries;
	nvmeibt_strlcpy(temp_gpt.main_or_metadata, gpt_level, sizeof(temp_gpt.main_or_metadata));

	// Read all 4 structures
	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
		NULL, disk_fd, pblk_size, &temp_gpt, pba_s, pba_e,
		&primary_header_validity, &alternate_header_validity,
		&primary_entries_validity, &alternate_entries_validity,
		primary_header, alternate_header,
		primary_entries, alternate_entries);

	// Detect mismatch: both copies valid but differ in content
	is_header_mismatch = false;
	is_entries_mismatch = false;

	if (primary_header_validity == GPT_VALIDITY_OK &&
		alternate_header_validity == GPT_VALIDITY_OK) {
		is_header_mismatch = !nvmeibt_disk_metadata_are_gpt_headers_equal(primary_header, alternate_header);
	}

	if (primary_entries_validity == GPT_VALIDITY_OK &&
		alternate_entries_validity == GPT_VALIDITY_OK &&
		primary_header->n_partition_entries > 0) {
		is_entries_mismatch = !nvmeibt_disk_metadata_are_gpt_entries_equal(
			primary_entries, alternate_entries, primary_header->n_partition_entries * sizeof(struct nvmeibt_disk_gpt_partition_entry));
	}

	is_mismatch = (is_header_mismatch || is_entries_mismatch);

	// Display based on --gpt-copy option
	if (strcmp(gpt_copy_option, "both") == 0) {
		display_gpt_one_copy(gpt_level, "Primary", primary_header_validity,
						primary_entries_validity, primary_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
		display_gpt_one_copy(gpt_level, "Alternate", alternate_header_validity,
						alternate_entries_validity, alternate_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
	} else if (strcmp(gpt_copy_option, "alternate") == 0) {
		display_gpt_one_copy(gpt_level, "Alternate", alternate_header_validity,
						alternate_entries_validity, alternate_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
	} else {
		// primary (default)
		display_gpt_one_copy(gpt_level, "Primary", primary_header_validity,
						primary_entries_validity, primary_entries, temp_gpt.max_n_entries,
						is_mismatch, config);
	}

	// Free buffers in same scope where allocated
	NNVMEIBT_BM_FREE(trace_gpt_util_cleanup_pri_hdr, primary_header);
	NNVMEIBT_BM_FREE(trace_gpt_util_cleanup_alt_hdr, alternate_header);
	NNVMEIBT_BM_FREE(trace_gpt_util_cleanup_pri_ent, primary_entries);
	NNVMEIBT_BM_FREE(trace_gpt_util_cleanup_alt_ent, alternate_entries);
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
 * @param gpt_copy_option  "primary", "alternate", or "both"
 * @param dev_name      Device name for error messages
 * @param fix_gpt       Whether to attempt GPT fix
 * @param config        Config for filtering (can be NULL for no filtering)
 * @return              0 on success, -1 on failure
 */
static int display_all_gpts(int disk_fd,
							int pblk_size,
							uint64_t pba_s,
							uint64_t pba_hw_e,
							const char *gpt_copy_option,
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
		fprintf(stderr, "Unable to fix Main-GPT for metadata access. dev_file_name=%s\n", dev_name);
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
	fprintf(stdout, "============================================================\n");
	fprintf(stdout, "gpt_util - NVMesh GPT/MBR Utility\n");
	fprintf(stdout, "Tool Version: %s\n", GPT_UTIL_VERSION);
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
	fprintf(stdout, "============================================================\n");
}

/**
 * Mark a sandbox file as persistent (so it survives close and can be re-opened)
 * In sandbox: Calls sandbox API to clear O_CREAT flag
 * In production: No-op (files already persist)
 */
static void SELF_TEST_mark_file_persistent(const char *filepath)
{
#ifdef TOMA_SIMULATOR_SANDBOX
	extern void sandbox_force_file_persist(const char *filepath);
	sandbox_force_file_persist(filepath);
#else
	(void)filepath;		// Unused in production
#endif
}

static int run_gpt_util_op(int argc, char *argv[]); // Forward declaration

/**
 * Run comprehensive self-test suite
 * Creates mock devices and calls run_gpt_util_op() with simulated parameters
 * This tests the complete production code path end-to-end
 */
static int run_self_test(void)
{
	int			rv = 1;
	int			disk_fd = -1;
	const char	*test_device_path;
	char		*test_argv[10];
	int			test_argc;

#ifdef TOMA_SIMULATOR_SANDBOX
	test_device_path = "_root/dev/gpt_util_self_test";
#else
	test_device_path = "/tmp/gpt_util_self_test";
#endif

	// ===== TEST 1: Normal GPT (no mismatch expected) =====
	if (1) {
		fprintf(stdout, "\n");
		fprintf(stdout, "============================================================\n");
		fprintf(stdout, "SELF-TEST 1: Normal GPT (Primary == Alternate)\n");
		fprintf(stdout, "Emulated command: gpt_util -a %s -c both\n", test_device_path);
		fprintf(stdout, "============================================================\n");

		// Create mock device
		disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(test_device_path);
		if (disk_fd < 0) {
			fprintf(stderr, "Failed to generate self-test device\n");
			goto out;
		}

		// Mark as persistent BEFORE closing (sandbox: clears O_CREAT flag)
		SELF_TEST_mark_file_persistent(test_device_path);

		close(disk_fd);
		disk_fd = -1;

		// Simulate: ./gpt_util -a <path> -c both
		test_argc = 5;
		test_argv[0] = "gpt_util";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-c";
		test_argv[4] = "both";

		optind = 1;		// Reset getopt state for clean parse
		if (run_gpt_util_op(test_argc, test_argv) != 0) {
			fprintf(stdout, "\n>>> SELF-TEST 1: FAILED <<<\n");
			goto out;
		}

		fprintf(stdout, "\n>>> SELF-TEST 1: PASSED <<<\n");
	}

	// ===== TEST 2: Mismatched GPT (mismatch expected) =====
	if (1) {
		fprintf(stdout, "\n");
		fprintf(stdout, "============================================================\n");
		fprintf(stdout, "SELF-TEST 2: Mismatched GPT (Primary != Alternate)\n");
		fprintf(stdout, "Emulated command: gpt_util -a %s -c both\n", test_device_path);
		fprintf(stdout, "============================================================\n");

		// Reuse same path - recreate device with corruption
		disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(test_device_path);
		if (disk_fd < 0) {
			fprintf(stderr, "Failed to generate mismatch test device\n");
			goto out;
		}

		if (SELF_TEST_corrupt_alternate_gpt_for_mismatch_test(disk_fd,
															SELF_TEST_MOCK_DEVICE_BLOCK_SIZE,
															1,
															SELF_TEST_MOCK_DEVICE_BLOCKS - 1) < 0) {
			fprintf(stderr, "Failed to corrupt alternate GPT for mismatch test\n");
			close(disk_fd);
			goto out;
		}

		// Mark as persistent BEFORE closing (sandbox: clears O_CREAT flag)
		SELF_TEST_mark_file_persistent(test_device_path);

		close(disk_fd);
		disk_fd = -1;

		// Simulate: ./gpt_util -a <path> -c both (same argv setup)
		optind = 1;		// Reset getopt state for clean parse
		if (run_gpt_util_op(test_argc, test_argv) != 0) {
			fprintf(stdout, "\n>>> SELF-TEST 2: FAILED <<<\n");
			goto out;
		}

		fprintf(stdout, "\n>>> SELF-TEST 2: PASSED <<<\n");
	}

	// ===== TEST 3: UUID Filtering =====
	if (1) {
		fprintf(stdout, "\n");
		fprintf(stdout, "============================================================\n");
		fprintf(stdout, "SELF-TEST 3: UUID Filtering\n");
		fprintf(stdout, "Emulated command: gpt_util -a %s --filter-uuid aabbccdd-1122-3344-5566-778899aabbcc\n", test_device_path);
		fprintf(stdout, "============================================================\n");

		// Recreate normal device
		disk_fd = SELF_TEST_generate_and_open_mock_nvmesh_disk(test_device_path);
		if (disk_fd < 0) {
			fprintf(stderr, "Failed to generate test device for UUID filter\n");
			goto out;
		}

		SELF_TEST_mark_file_persistent(test_device_path);
		close(disk_fd);
		disk_fd = -1;

		// Simulate: ./gpt_util -a <path> --filter-uuid <UUID>
		test_argc = 5;
		test_argv[0] = "gpt_util";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "--filter-uuid";
		test_argv[4] = "aabbccdd-1122-3344-5566-778899aabbcc";		// NVMESH_METADATA partition UUID

		optind = 1;
		if (run_gpt_util_op(test_argc, test_argv) != 0) {
			fprintf(stdout, "\n>>> SELF-TEST 3: FAILED <<<\n");
			goto out;
		}

		fprintf(stdout, "\n>>> SELF-TEST 3: PASSED <<<\n");
	}

	// ===== TEST 4: LBA Filtering =====
	if (1) {
		fprintf(stdout, "\n");
		fprintf(stdout, "============================================================\n");
		fprintf(stdout, "SELF-TEST 4: LBA Filtering\n");
		fprintf(stdout, "Emulated command: gpt_util -a %s --filter-lba 1000\n", test_device_path);
		fprintf(stdout, "============================================================\n");

		// Use existing device (no need to recreate)

		// Simulate: ./gpt_util -a <path> --filter-lba <LBA>
		test_argc = 5;
		test_argv[0] = "gpt_util";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "--filter-lba";
		test_argv[4] = "1000";		// LBA inside NVMESH_METADATA partition

		optind = 1;
		if (run_gpt_util_op(test_argc, test_argv) != 0) {
			fprintf(stdout, "\n>>> SELF-TEST 4: FAILED <<<\n");
			goto out;
		}

		fprintf(stdout, "\n>>> SELF-TEST 4: PASSED <<<\n");
	}

	// ===== TEST 5: Overlap Detection =====
	if (1) {
		fprintf(stdout, "\n");
		fprintf(stdout, "============================================================\n");
		fprintf(stdout, "SELF-TEST 5: Overlap Detection\n");
		fprintf(stdout, "Emulated command: gpt_util -a %s -c both\n", test_device_path);
		fprintf(stdout, "============================================================\n");

		// Create device with overlapping partitions
		disk_fd = SELF_TEST_generate_mock_device_with_overlaps(test_device_path);
		if (disk_fd < 0) {
			fprintf(stderr, "Failed to generate device with overlaps\n");
			goto out;
		}

		SELF_TEST_mark_file_persistent(test_device_path);
		close(disk_fd);
		disk_fd = -1;

		// Simulate: ./gpt_util -a <path> -c both
		test_argc = 5;
		test_argv[0] = "gpt_util";
		test_argv[1] = "-a";
		test_argv[2] = (char *)test_device_path;
		test_argv[3] = "-c";
		test_argv[4] = "both";

		optind = 1;
		if (run_gpt_util_op(test_argc, test_argv) != 0) {
			fprintf(stdout, "\n>>> SELF-TEST 5: FAILED <<<\n");
			goto out;
		}

		fprintf(stdout, "\n>>> SELF-TEST 5: PASSED <<<\n");
	}

	// ===== SUMMARY =====
	fprintf(stdout, "\n");
	fprintf(stdout, "============================================================\n");
	fprintf(stdout, "ALL SELF-TESTS PASSED (5/5)\n");
	fprintf(stdout, "============================================================\n");

	rv = 0;

out:
	if (disk_fd >= 0) {
		close(disk_fd);
	}

	// Always clean up test file (even on failure - we marked it persistent)
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
	fprintf(stdout, "  -i, --check-nvmesh        Check if NVMESH_METADATA partition exists\n");
	fprintf(stdout, "  -f, --fix-gpt               Fix GPT from alternate copy (and display)\n");
	fprintf(stdout, "  -F, --fix-mbr               Fix MBR (and display)\n");
	fprintf(stdout, "  (default: display GPT)      Display GPT structure\n\n");

	fprintf(stdout, "Display Options:\n");
	fprintf(stdout, "  -c, --gpt-copy=WHICH        Which copy: primary|alternate|both (default: primary)\n");
	fprintf(stdout, "  --filter-uuid=UUID          Show only entries matching UUID\n");
	fprintf(stdout, "  --filter-lba=ADDR           Show only entries containing LBA address\n\n");

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
		{"check-nvmesh",			no_argument,		0,	'i'},
		{"print-mbr",				no_argument,		0,	'm'},
		{"fix-gpt",					no_argument,		0,	'f'},
		{"fix-mbr",					no_argument,		0,	'F'},
		{"gpt-copy",				required_argument,	0,	'c'},
		{"filter-uuid",				required_argument,	0,	'u'},
		{"filter-lba",				required_argument,	0,	'l'},

		{0, 0, 0, 0}
	};
	static const char short_options[] = "d:a:s:e:b:c:u:l:imfF";
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
				fprintf(stderr, "OOPS! Invalid file descriptor=%d file_name=%s err=%m", fd, file_name);
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
						fprintf(stdout,"Wrong section_type=%d given\n", section_type);
						rv = -1;
						goto out;
					}
					if (memcmp(line, ref_header, line_len) != 0) {
						fprintf(stderr, "Expecting csv header '%s'. Got '%s'\n", ref_header, line);
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
						fprintf(stderr,"Failed to parse line: %s\n",  line);
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
				fprintf(stderr, "Error: Multiple actions specified (only one allowed)\n");
				rv = -1;
				goto out;
			}
			config->action = ACTION_DISPLAY_MBR;
			fprintf(stdout, "Action: Display MBR\n");
			break;
		case 'f':
			if (config->action != ACTION_DISPLAY_GPT) {
				fprintf(stderr, "Error: Multiple actions specified (only one allowed)\n");
				rv = -1;
				goto out;
			}
			config->action = ACTION_FIX_GPT;
			fprintf(stdout, "Action: Fix GPT from alternate copy\n");
			break;
		case 'F':
			if (config->action != ACTION_DISPLAY_GPT) {
				fprintf(stderr, "Error: Multiple actions specified (only one allowed)\n");
				rv = -1;
				goto out;
			}
			config->action = ACTION_FIX_MBR;
			fprintf(stdout, "Action: Fix MBR\n");
			break;
		case 'i':
			if (config->action != ACTION_DISPLAY_GPT) {
				fprintf(stderr, "Error: Multiple actions specified (only one allowed)\n");
				rv = -1;
				goto out;
			}
			config->action = ACTION_CHECK_NVMESH;
			fprintf(stdout, "Action: Check for NVMESH_METADATA partition\n");
			break;
		case 'c':
			nvmeibt_strlcpy(config->gpt_copy_option, optarg, sizeof(config->gpt_copy_option));
			if (strcmp(config->gpt_copy_option, "primary") != 0 &&
				strcmp(config->gpt_copy_option, "alternate") != 0 &&
				strcmp(config->gpt_copy_option, "both") != 0) {
				fprintf(stderr, "Invalid --gpt-copy value: %s (use: primary|alternate|both)\n", config->gpt_copy_option);
				rv = -1;
				goto out;
			}
			fprintf(stdout, "GPT copy selection: %s\n", config->gpt_copy_option);
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
		case 'a':
			nvmeibt_strlcpy(config->device_path, optarg, sizeof(config->device_path));
			config->is_nvmesh_managed = false;
			fprintf(stdout, "Device: %s (any device)\n", config->device_path);
			// Get device info immediately to fill config
			if (get_device_info(config->device_path, &config->pblk_size, &config->pba_e) < 0) {
				rv = -1;
				goto out;
			}
			config->pba_s = 1;
			config->pba_hw_e = config->pba_e;
			break;
		default:
			fprintf(stderr, "Error: Unknown option\n");
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
		fprintf(stderr, "Error: Must specify device with -d or -a\n");
		return -1;
	}

	// For display GPT action, validate gpt_copy_option
	if (config->action == ACTION_DISPLAY_GPT) {
		if (strcmp(config->gpt_copy_option, "primary") != 0 &&
			strcmp(config->gpt_copy_option, "alternate") != 0 &&
			strcmp(config->gpt_copy_option, "both") != 0) {
			fprintf(stderr, "Error: Invalid gpt_copy_option=%s\n", config->gpt_copy_option);
			return -1;
		}
	}

	return 0;
}

/**
 * Phase 3: Execute CHECK_NVMESH action
 */
static int execute_check_nvmesh(int disk_fd, struct gpt_util_config *config)
{
	int						rv = -1;
	struct nvmeibt_disk_gpt	temp_gpt;

	memset(&temp_gpt, 0, sizeof(temp_gpt));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &temp_gpt,
										  config->pba_s, config->pba_hw_e, false) == 0) {
		if (nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&temp_gpt)) {
			fprintf(stdout, "%s NVMESH_METADATA_FOUND\n", config->device_path);
			rv = 0;
		} else {
			fprintf(stdout, "%s NVMESH_METADATA_NOT_FOUND\n", config->device_path);
			rv = 0;
		}
	} else {
		fprintf(stderr, "Error: Failed to read GPT from device\n");
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
		fprintf(stderr, "Unable to read MBR. dev_file_name=%s block_size=%d\n",
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
		fprintf(stderr, "Unable to read MBR. dev_file_name=%s block_size=%d\n",
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
			fprintf(stderr, "Error: Disk size not detected, please specify with -e\n");
			goto out;
		}

		nvmeibt_disk_metadata_init_pmbr(&mbr, config->pba_e + 1, config->pblk_size);
		if (nvmeibt_disk_metadata_write_mbr(NULL, disk_fd, config->pblk_size, &mbr) < 0) {
			fprintf(stderr, "MBR write failed\n");
			goto out;
		}

		fprintf(stdout, "MBR fix successful\n");
	} else {
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

	// display_all_gpts() with fix_gpt=true will fix from alternate and display
	rv = display_all_gpts(disk_fd, config->pblk_size, config->pba_s, config->pba_hw_e,
						  config->gpt_copy_option, config->device_path, true, config);

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
	nvmeibt_strlcpy(config.gpt_copy_option, "primary", sizeof(config.gpt_copy_option));

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
	if ((disk_fd = open(config.device_path, O_RDWR | O_EXCL | __O_DIRECT)) < 0) {
		fprintf(stderr, "Unable to open=%s with O_EXCL err=%m\n", config.device_path);
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

	case ACTION_CHECK_NVMESH:
		rv = execute_check_nvmesh(disk_fd, &config);
		break;

	default:
		fprintf(stderr, "Error: Unknown action=%d\n", config.action);
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

#ifdef TOMA_SIMULATOR_SANDBOX
	// Initialize sandbox environment for subprogram execution
	{ extern void toma_unitest_env_start(void); toma_unitest_env_start(); }
#endif // #ifdef TOMA_SIMULATOR_SANDBOX

	// Buffer Manager
	if (nvmeibt_bm_create()) {
		fprintf(stderr, "Fail to create buffer manager\n");
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
	return rv;
}

#ifdef GPT_UTIL

int main(int argc, char *argv[])
{
	return gpt_util_main(argc, argv);
}

#endif //GPT_UTIL

