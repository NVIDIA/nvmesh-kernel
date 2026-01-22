/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/**
 * gpt_util - NVMesh GPT/MBR Utility
 *
 * Displays, exports, and applies GPT structures for NVMesh-managed and regular block devices.
 *
 * This tool may run when TOMA is up, but only in read-only mode and when the target device is unbound from nvmeibs.
 *
 * For write mode or self-test mode, TOMA has to be stopped.
 */

#include "../nvmeibt_debug.h"
#include <ctype.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gpt_util_self_test.h"

#include "nvmeibt_utils.h"
#include "../nvmeibt_toma.h"
#include "../nvmeibt_disk_metadata.h"
#include "nvmeibt_bm.h"
#include "../nvmeibt_read_config.h"
#include "nvmeibt_str.h"
#include "../nvmeibt_local_disk.h"
#include "nvmeibt_uuid.h"
#include "../nvmeibt_json_base.h"
#include "../interfaces/log/nvmeibt_binary_tracing.h"
#include "../interfaces/nvme/nvmeibt_nvme_defines.h"
#include "../nvmeibt_rpc.h"
#include "../nvmeibt_ds_metadata.h"


#define GPT_UTIL_VERSION	"2.0.2-dev"
#define MAX_DEV_NAME		256

// Actions (mutually exclusive operations)
enum GPT_UTIL_ACTION {
	ACTION_DISPLAY_GPT = 0,		// Default: display GPT structure
	ACTION_DISPLAY_MBR,			// -m: display MBR only
	ACTION_FIX_GPT,				// -f: fix GPT from alternate copy
	ACTION_FIX_MBR,				// -F: fix MBR
	ACTION_CHECK_NVMESH		// -i: check if NVMESH_METADATA exists
	ACTION_EXPORT_JSON,			// --output-json: export GPT to JSON
	ACTION_APPLY_JSON,			// --apply-from: apply GPT from JSON (dry-run by default)
	ACTION_UPGRADE_GPT,			// -U: upgrade GPT (fix n_partition_entries to 8192 and recalculate CRC)
	ACTION_RESTORE_BINARY		// --restore-binary: restore from binary backup
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

	// Binary restore options (for ACTION_RESTORE_BINARY)
	char					restore_binary_file[256];	// Binary backup file to restore

	// Confirmation options
	BOOL					skip_confirmation;		// Skip interactive confirmation (--yes flag)

	// I/O options
	enum O_DIRECT_MODE		o_direct_mode;			// O_DIRECT behavior

	// Self-test mode
	BOOL					is_self_test;			// true if running in self-test mode
};

// TOMA single-instance lock state for gpt_util. This enum does not capture the state when TOMA is running.
typedef enum {
	TOMA_LOCK_NOT_HELD_BY_GPT,		// No lock held by gpt_util or its self-test framework
	TOMA_LOCK_GPT_UTIL,				// Lock held by normal gpt_util operation (release at end)
	TOMA_LOCK_GPT_SELF_TEST,		// Lock held by gpt_util self-test framework (don't release during ops)
	TOMA_LOCK_MOCK_RUNNING,			// Self-test: pretend TOMA is running
} toma_lock_state_gpt_t;

static toma_lock_state_gpt_t s_toma_lock_state_gpt = TOMA_LOCK_NOT_HELD_BY_GPT;

/**
 * Checks if TOMA is running. If not, acquires the single instance lock.
 * Returns true if TOMA is running or mocked, false if not running.
 */
static BOOL is_toma_running_and_acquire_lock(void)
{
	if (s_toma_lock_state_gpt == TOMA_LOCK_MOCK_RUNNING) {
		return true;	// Mocked TOMA running always overrides the lock check
	}
	if (s_toma_lock_state_gpt == TOMA_LOCK_GPT_UTIL || s_toma_lock_state_gpt == TOMA_LOCK_GPT_SELF_TEST) {
		return false;	// Already holding lock by gpt_util or its self-test framework, so TOMA is not running
	}
	if (nvmeibt_toma_is_single_instance() < 0) {
		return true;	// Real TOMA is running
	}
	s_toma_lock_state_gpt = TOMA_LOCK_GPT_UTIL;
	return false;
}

// Releases lock if acquired by normal operation (not if self-test owns it).
static void release_toma_lock_if_acquired(void)
{
	if (s_toma_lock_state_gpt == TOMA_LOCK_GPT_UTIL) {
		nvmeibt_toma_cleanup_single_instance();
		s_toma_lock_state_gpt = TOMA_LOCK_NOT_HELD_BY_GPT;
	}
}

/**
 * Below are toma lock functions only used by self-test framework.
 * Do NOT call them in places other than self-test framework.
 */
// Acquires lock for test framework, to prevent self-test from interfering with real TOMA. Returns true if real TOMA is running.
BOOL SELF_TEST_acquire_toma_lock(void)
{
	if (nvmeibt_toma_is_single_instance() < 0) {
		return true;
	}
	s_toma_lock_state_gpt = TOMA_LOCK_GPT_SELF_TEST;
	return false;
}
// Releases lock held by self-test and reset state.
void SELF_TEST_release_toma_lock(void)
{
	NTOMA_ASSERT(error_self_test_release_toma_lock, s_toma_lock_state_gpt != TOMA_LOCK_GPT_UTIL, "Self test cannot release lock really held by gpt_util");
	if (s_toma_lock_state_gpt != TOMA_LOCK_NOT_HELD_BY_GPT) {
		nvmeibt_toma_cleanup_single_instance();
		s_toma_lock_state_gpt = TOMA_LOCK_NOT_HELD_BY_GPT;
	}
}
// Mocks TOMA as running.
void SELF_TEST_mock_toma_running(void)
{
	s_toma_lock_state_gpt = TOMA_LOCK_MOCK_RUNNING;
}
// Undoes mock, back to lock held by self-test framework.
void SELF_TEST_undo_mock_toma_running(void)
{
	s_toma_lock_state_gpt = TOMA_LOCK_GPT_SELF_TEST;
}

/**
 * Create private backup directory with restrictive permissions
 * Returns 0 on success, -1 on error
 */
static int create_backup_directory(const char *backup_dir)
{
	struct stat	st;

	/* Check if directory already exists */
	if (stat(backup_dir, &st) == 0) {
		/* Directory exists - verify it's a directory and has correct permissions */
		if (!S_ISDIR(st.st_mode)) {
			N_Ef(backup_dir_not_dir, "Backup path exists but is not a directory: @STR", backup_dir);
			fprintf(stderr, COL_RED_BOLD "ERROR: Backup path exists but is not a directory: %s" COL_RESET "\n", backup_dir);
			return -1;
		}
		/* Directory exists and is valid */
		return 0;
	}

	/* Create directory with restrictive permissions (0700 - owner only) */
	if (mkdir(backup_dir, 0700) < 0) {
		N_Ef(backup_dir_create_failed, "Failed to create backup directory @STR @AUTO_ERRNO", backup_dir);
		fprintf(stderr, COL_RED_BOLD "ERROR: Failed to create backup directory: %s" COL_RESET "\n", backup_dir);
		return -1;
	}

	N_Tf(backup_dir_created, "Created backup directory: @STR", backup_dir);
	return 0;
}

/**
 * Helper: Backup a single structure to hidden file
 * Returns 0 on success, -1 on error
 */
static int backup_structure(int disk_fd, const char *filepath, uint64_t pba_start, uint64_t n_blocks, int pblk_size)
{
	int			rv = -1;
	int			backup_fd = -1;
	char		*backup_buffer = NULL;
	uint64_t	backup_size_bytes;
	uint64_t	pbyte_start;

	backup_size_bytes = n_blocks * pblk_size;
	pbyte_start = pba_start * pblk_size;

	backup_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_backup_struct_buf, PAGE_SIZE, backup_size_bytes);
	if (!backup_buffer) {
		N_Ef(backup_struct_alloc_failed, "Failed to allocate backup buffer size=@SIZE_T", backup_size_bytes);
		goto out;
	}

	/* Use TOMA I/O helper for robust reading (handles EINTR, partial I/O) */
	if (NNVMEIBT_PREAD_ATOMIC(trace_backup_struct_pread, disk_fd, backup_buffer, backup_size_bytes, pbyte_start, 1) != (ssize_t)backup_size_bytes) {
		N_Ef(backup_struct_read_failed, "Failed to read structure pba=@ZX", pba_start);
		goto out;
	}

	/* Open with O_EXCL | O_NOFOLLOW to prevent symlink attacks */
	backup_fd = open(filepath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (backup_fd < 0) {
		N_Ef(backup_struct_open_failed, "Failed to create @STR @AUTO_ERRNO", filepath);
		goto out;
	}

	if (write(backup_fd, backup_buffer, backup_size_bytes) != (ssize_t)backup_size_bytes) {
		N_Ef(backup_struct_write_failed, "Failed to write @STR @AUTO_ERRNO", filepath);
		goto out;
	}

	/* fsync ensures backup is durable before returning success */
	if (fsync(backup_fd) < 0) {
		N_Ef(backup_struct_fsync_failed, "Failed to sync @STR @AUTO_ERRNO", filepath);
		goto out;
	}

	rv = 0;

out:
	if (backup_fd >= 0) {
		close(backup_fd);
	}
	NNVMEIBT_BM_FREE(trace_backup_struct_buf_free, backup_buffer);
	return rv;
}

/**
 * Helper: Backup structure and append to manifest JSON
 * Returns 0 on success, -1 on error
 */
static int backup_structure_and_append_manifest(int disk_fd,
												const char *structure_name,
												const char *backup_dir,
												uint64_t pba_start,
												uint64_t n_blocks,
												int pblk_size,
												struct nvmeibt_Str *manifest_json,
												uint64_t *total_backup_bytes,
												BOOL is_first)
{
	char	structure_file[512];
	int		rv;

	/* Build structure file path within private backup directory */
	snprintf(structure_file, sizeof(structure_file), "%s/%s.bin",
			 backup_dir, structure_name);

	/* Backup the structure */
	rv = backup_structure(disk_fd, structure_file, pba_start, n_blocks, pblk_size);
	if (rv < 0) {
		return rv;
	}

	/* Update total bytes */
	*total_backup_bytes += n_blocks * pblk_size;

	/* Append to manifest JSON */
	if (!is_first) {
		nvmeibt_Str_sprintf(manifest_json, ",\n");
	}
	nvmeibt_Str_sprintf(manifest_json, "    {\n");
	nvmeibt_Str_sprintf(manifest_json, "      \"name\": \"%s\",\n", structure_name);
	nvmeibt_Str_sprintf(manifest_json, "      \"file\": \"%s\",\n", structure_file);
	nvmeibt_Str_sprintf(manifest_json, "      \"pba_start\": %lu,\n", pba_start);
	nvmeibt_Str_sprintf(manifest_json, "      \"n_blocks\": %lu\n", n_blocks);
	nvmeibt_Str_sprintf(manifest_json, "    }");

	return 0;
}

/**
 * Format in-memory GPT structures as JSON fields (merge-ready).
 * Outputs just the memory_* fields that can be directly inserted into a larger JSON object.
 * Does NOT include outer braces - caller must handle JSON object boundaries.
 *
 * @param out         Output string buffer
 * @param local_disk  Local disk structure containing GPT, MBR, and change tracking
 */
void gpt_util_format_memory_gpt_json(struct nvmeibt_Str *out,
									 const struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_urn_uuid					urn_uuid;
	int										entry_count = 0;
	const struct nvmeibt_disk_gpt			*main_gpt = &local_disk->main_gpt;
	const struct nvmeibt_disk_gpt			*metadata_gpt = &local_disk->metadata_gpt;
	const struct nvmeibt_disk_mbr			*mbr = &local_disk->mbr;

	/* Export change tracking info */
	nvmeibt_Str_sprintf(out, "  \"gpt_change_no\": %d,\n", local_disk->gpt_change_no);
	nvmeibt_Str_sprintf(out, "  \"gpt_submitted_change_no\": %d,\n", local_disk->gpt_submitted_change_no);
	nvmeibt_Str_sprintf(out, "  \"has_pending_writes\": %s,\n",
						(local_disk->gpt_change_no > local_disk->gpt_submitted_change_no) ? "true" : "false");

	/* Export MBR */
	nvmeibt_Str_sprintf(out, "  \"memory_mbr\": {\n");
	nvmeibt_Str_sprintf(out, "    \"signature\": \"0x%04x\",\n", (unsigned short)mbr->signature);
	nvmeibt_Str_sprintf(out, "    \"os_type\": \"0x%02x\",\n", (unsigned char)mbr->partitions[0].os_type);
	nvmeibt_Str_sprintf(out, "    \"pba_s\": %d,\n", mbr->partitions[0].pba_s);
	nvmeibt_Str_sprintf(out, "    \"n_pblk\": %d\n", mbr->partitions[0].n_pblk);
	nvmeibt_Str_sprintf(out, "  },\n");

	/* Export Main GPT */
	nvmeibt_Str_sprintf(out, "  \"memory_main_gpt\": {\n");
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&main_gpt->header.disk_obj_uuid);
	nvmeibt_Str_sprintf(out, "    \"disk_uuid\": \"%s\",\n", urn_uuid.str);
	nvmeibt_Str_sprintf(out, "    \"first_usable_pba\": %lu,\n", main_gpt->header.first_usable_pba);
	nvmeibt_Str_sprintf(out, "    \"last_usable_pba\": %lu,\n", main_gpt->header.last_usable_pba);
	nvmeibt_Str_sprintf(out, "    \"n_partition_entries\": %d,\n", main_gpt->header.n_partition_entries);
	nvmeibt_Str_sprintf(out, "    \"header_crc32\": \"0x%08x\",\n", main_gpt->header.header_crc32);
	nvmeibt_Str_sprintf(out, "    \"partition_entry_array_crc32\": \"0x%08x\",\n", main_gpt->header.partition_entry_array_crc32);
	nvmeibt_Str_sprintf(out, "    \"entries\": [\n");

	entry_count = 0;
	for (int i = 0; i < main_gpt->max_n_entries; i++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&main_gpt->entries[i])) {
			const struct nvmeibt_disk_gpt_partition_entry *entry = &main_gpt->entries[i];
			struct nvmeibt_urn_uuid type_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_type_guid);
			struct nvmeibt_urn_uuid part_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_guid);
			char partition_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];

			char16_str_to_str(entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name);

			if (entry_count > 0) {
				nvmeibt_Str_sprintf(out, ",\n");
			}
			nvmeibt_Str_sprintf(out, "      {\"index\":%d,\"type_guid\":\"%s\",\"partition_guid\":\"%s\",\"pba_s\":%lu,\"pba_e\":%lu,\"attributes\":%lu,\"name\":\"%s\"}",
								i, type_urn.str, part_urn.str, entry->pba_s, entry->pba_e, entry->attributes, partition_name);
			entry_count++;
		}
	}
	nvmeibt_Str_sprintf(out, "\n    ]\n");
	nvmeibt_Str_sprintf(out, "  },\n");

	/* Export Metadata GPT */
	nvmeibt_Str_sprintf(out, "  \"memory_metadata_gpt\": {\n");
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&metadata_gpt->header.disk_obj_uuid);
	nvmeibt_Str_sprintf(out, "    \"disk_uuid\": \"%s\",\n", urn_uuid.str);
	nvmeibt_Str_sprintf(out, "    \"first_usable_pba\": %lu,\n", metadata_gpt->header.first_usable_pba);
	nvmeibt_Str_sprintf(out, "    \"last_usable_pba\": %lu,\n", metadata_gpt->header.last_usable_pba);
	nvmeibt_Str_sprintf(out, "    \"n_partition_entries\": %d,\n", metadata_gpt->header.n_partition_entries);
	nvmeibt_Str_sprintf(out, "    \"header_crc32\": \"0x%08x\",\n", metadata_gpt->header.header_crc32);
	nvmeibt_Str_sprintf(out, "    \"partition_entry_array_crc32\": \"0x%08x\",\n", metadata_gpt->header.partition_entry_array_crc32);
	nvmeibt_Str_sprintf(out, "    \"entries\": [\n");

	entry_count = 0;
	for (int i = 0; i < metadata_gpt->max_n_entries; i++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_gpt->entries[i])) {
			const struct nvmeibt_disk_gpt_partition_entry *entry = &metadata_gpt->entries[i];
			struct nvmeibt_urn_uuid type_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_type_guid);
			struct nvmeibt_urn_uuid part_urn = nvmeibt_union_uuid_to_urn_uuid(&entry->partition_guid);
			char partition_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];

			char16_str_to_str(entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, partition_name);

			if (entry_count > 0) {
				nvmeibt_Str_sprintf(out, ",\n");
			}
			nvmeibt_Str_sprintf(out, "      {\"index\":%d,\"type_guid\":\"%s\",\"partition_guid\":\"%s\",\"pba_s\":%lu,\"pba_e\":%lu,\"attributes\":%lu,\"name\":\"%s\"}",
								i, type_urn.str, part_urn.str, entry->pba_s, entry->pba_e, entry->attributes, partition_name);
			entry_count++;
		}
	}
	nvmeibt_Str_sprintf(out, "\n    ]\n");
	nvmeibt_Str_sprintf(out, "  }\n");
}

/**
 * Self-test mock for memory GPT without real TOMA RPC.
 * Once set, get_memory_gpt_via_rpc() returns this instead of calling RPC
 */
static const struct nvmeibt_local_disk		*s_mock_local_disk = NULL;
void SELF_TEST_set_mock_local_disk(const struct nvmeibt_local_disk *local_disk)
{
	s_mock_local_disk = local_disk;
}

/**
 * Get memory GPT from TOMA via RPC and return as JSON string (merge-ready).
 * Uses toma_rpc CLI tool via popen() to avoid code duplication.
 * In self-test mode with mock local_disk, generates JSON from mock local disk.
 *
 * @param device_path     Device path to query
 * @param out             Output buffer for JSON fields (caller-provided)
 * @param is_self_test    true if in self-test mode (use mock)
 * @return 0 on success, -1 on error
 */
static int get_memory_gpt_via_rpc(const char *device_path,
								   struct nvmeibt_Str *out,
								   BOOL is_self_test)
{
	char					command[512];
	char					response[65536];		// Large enough for GPT JSON
	int						n_read = 0;
	int						rv = -1;
	FILE					*pipe_fp = NULL;
	int						pclose_status;
	char					*trailer;

	/* Check for mock data (self-test mode) */
	if (is_self_test && s_mock_local_disk) {
		N_Tf(rpc_using_mock, "Using mock local_disk for testing");
		gpt_util_format_memory_gpt_json(out, s_mock_local_disk);	// Generates json string of memory GPT data from mock local_disk. This is the function used by TOMA RPC handler as well.
		return 0;
	}

	/* Build command: toma_rpc export-memory-gpt <device> */
	snprintf(command, sizeof(command), "%s export-memory-gpt %s 2>/dev/null",
			 TOMA_RPC_TOOL_PATH, device_path);

	pipe_fp = popen(command, "r");
	if (!pipe_fp) {
		N_Ef(rpc_popen_failed, "Failed to popen toma_rpc: @AUTO_ERRNO");
		goto out;
	}

	/* Read entire response into buffer */
	n_read = fread(response, 1, sizeof(response) - 1, pipe_fp);
	response[n_read] = '\0';

	pclose_status = pclose(pipe_fp);
	pipe_fp = NULL;

	if (pclose_status != 0 || n_read <= 0) {
		N_Ef(rpc_send_failed, "toma_rpc failed: status=@INT n_read=@INT", pclose_status, n_read);
		goto out;
	}

	/* Strip toma_rpc trailer: "[end Nb]" or "[error %m]" at end of output */
	trailer = strstr(response, "\n[end ");
	if (trailer) {
		*trailer = '\0';
		n_read = trailer - response;
	}

	/* Check for error responses */
	if (strncmp(response, "ERROR:", 6) == 0) {
		N_Tf(rpc_error_response, "TOMA RPC returned error: @STR", response);
		goto out;
	}

	N_Tf(rpc_response_received, "RPC response: @INT bytes", n_read);

	/*
	 * TOMA RPC response contains memory GPT fields (merge-ready format).
	 * No outer braces - just the fields that can be directly inserted into our JSON.
	 * Simply copy the response to output.
	 */
	nvmeibt_Str_sprintf(out, "%s", response);
	rv = 0;

out:
	return rv;
}

/**
 * Get device serial number for validation
 * Strategy:
 *   1. Try NVMe controller ioctl (works for real NVMe devices and registered sandbox devices)
 *   2. If that fails AND is_self_test mode, generate stable mock serial from path
 * Returns 0 on success, -1 on error
 */
static int get_device_serial_num(int fd, struct gpt_util_config *config, char *serial_out, size_t size)
{
	struct nvme_id_ctrl		*id_ctrl = NULL;
	struct nvme_admin_cmd	cmd;
	int						rv = -1;
	int						len;
	struct stat				st;

	/* Try NVMe controller identify ioctl first */
	id_ctrl = NNVMEIBT_BM_ALIGNED_CALLOC(trace_nvme_get_serial, PAGE_SIZE, sizeof(*id_ctrl));
	if (!id_ctrl) {
		N_Ef(nvme_serial_alloc_failed, "Failed to allocate buffer for NVMe identify");
		goto fallback_to_mock;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_identify;
	cmd.nsid = 0;		/* 0 = controller identify */
	cmd.addr = (__u64)(uintptr_t)id_ctrl;
	cmd.data_len = sizeof(*id_ctrl);
	cmd.cdw10 = 1;		/* CNS=1 for controller identify */

	rv = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (rv == 0) {
		/* NVMe ioctl succeeded - trim whitespace from serial */
		int i;
		len = strnlen(id_ctrl->sn, sizeof(id_ctrl->sn));
		for (i = len - 1; i >= 0; i--) {
			if (id_ctrl->sn[i] > ' ') {
				break;
			}
		}
		len = i + 1;

		if (len == 0) {
			N_Wf(nvme_serial_empty, "NVMe controller serial is empty");
			goto fallback_to_mock;
		}

		if ((size_t)len >= size) {
			N_Ef(nvme_serial_too_long, "Serial number too long: @INT bytes (max @SIZE_T)", len, size - 1);
			goto fallback_to_mock;
		}

		memcpy(serial_out, id_ctrl->sn, len);
		serial_out[len] = '\0';

		N_Tf(nvme_serial_retrieved, "NVMe controller serial: @STR (len=@INT)", serial_out, len);
		rv = 0;
		goto out;
	}

fallback_to_mock:
	/* NVMe ioctl failed - only allow mock serial in self-test mode */
	if (!config->is_self_test) {
		N_Ef(serial_not_nvme_device, "Device is not NVMe or ioctl failed: @STR", config->device_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read NVMe controller serial number" COL_RESET "\n");
		fprintf(stderr, "  Device: %s\n", config->device_path);
		rv = -1;
		goto out;
	}

	/* Self-test mode - check if this is a regular file (test device) */
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
		/* Regular file (sandbox test device or test file) - generate stable mock serial */
		SELF_TEST_generate_mock_serial_number_from_path(config->device_path, serial_out, size);
		N_Tf(mock_serial_generated, "Generated mock serial for test file: path=@STR serial=@STR", config->device_path, serial_out);
		rv = 0;
		goto out;
	}

	/* Neither NVMe device nor regular file - fail */
	N_Ef(serial_unsupported_device, "Cannot get serial for device: @STR", config->device_path);
	rv = -1;

out:
	NNVMEIBT_BM_FREE(trace_nvme_get_serial_free, id_ctrl);
	return rv;
}

/**
 * Load and parse JSON file into tree.
 * Returns json_root on success, NULL on failure (logs errors internally).
 *
 * @param filepath   Path to JSON file
 * @return JSON root element (dict), or NULL if failed
 */
static struct mm_json_elem *load_json_file_or_fail(const char *filepath)
{
	int						json_fd = -1;
	struct nvmeibt_Str		*json_content = NULL;
	struct mm_json_elem		*json_root = NULL;

	json_fd = NNVMEIBT_OPEN_READ(trace_load_json, filepath, 1);
	if (json_fd < 0) {
		N_Ef(load_json_open_failed, "Cannot open JSON file @STR @AUTO_ERRNO", filepath);
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot open JSON file: %s" COL_RESET "\n", filepath);
		goto out;
	}

	json_content = NNVMEIBT_STR_ALLOC(trace_load_json_content);
	if (NNVMEIBT_STR_FREAD_ATOMIC(trace_load_json_read, json_content, json_fd) < 0) {
		N_Ef(load_json_read_failed, "Cannot read JSON file @STR @AUTO_ERRNO", filepath);
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read JSON file: %s" COL_RESET "\n", filepath);
		goto out;
	}

	json_root = parse_json_txt_into_kv_tree(nvmeibt_Str_str(json_content), nvmeibt_Str_strlen(json_content));
	if (!json_root || json_root->type != JSON_E_DICT) {
		N_Ef(load_json_parse_failed, "Invalid JSON format @STR", filepath);
		fprintf(stderr, COL_RED_BOLD "ERROR: Invalid JSON format: %s" COL_RESET "\n", filepath);
		if (json_root) {
			nvmeibt_mm_json_free_kv_tree(json_root);
			json_root = NULL;
		}
		goto out;
	}

out:
	if (json_fd >= 0) {
		NNVMEIBT_CLOSE(trace_load_json_close, json_fd);
	}
	NNVMEIBT_STR_FREE(trace_load_json_free, json_content);
	return json_root;
}

/**
 * Validate that source serial number matches current device serial number.
 * Returns 0 if match, -1 if mismatch or error (with detailed logging).
 *
 * @param disk_fd         Device file descriptor
 * @param config          Config for reading serial
 * @param source_serial   Expected serial number (from JSON/manifest)
 * @param source_name     Name of source for error messages (e.g., "JSON", "manifest")
 */
static int validate_serial_number_match(int disk_fd, struct gpt_util_config *config,
										const char *source_serial, const char *source_name)
{
	char	current_serial[64] = {0};

	/* Validate source serial is present */
	if (!source_serial || strlen(source_serial) == 0) {
		N_Ef(validate_serial_missing, "@STR missing serial number", source_name);
		fprintf(stderr, COL_RED_BOLD "ERROR: %s missing serial - validation blocked" COL_RESET "\n", source_name);
		return -1;
	}

	/* Get current device serial */
	if (get_device_serial_num(disk_fd, config, current_serial, sizeof(current_serial)) < 0) {
		N_Ef(validate_serial_read_failed, "Cannot read device serial");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read device serial - validation blocked" COL_RESET "\n");
		return -1;
	}

	/* Compare serials */
	if (strcmp(source_serial, current_serial) != 0) {
		N_Ef(validate_serial_mismatch, "Serial mismatch: @STR=@STR device=@STR",
				source_name, source_serial, current_serial);
		fprintf(stderr, COL_RED_BOLD "ERROR: Serial mismatch! %s=%s device=%s" COL_RESET "\n",
				source_name, source_serial, current_serial);
		return -1;
	}

	N_Tf(validate_serial_match, "Serial validation passed: @STR", source_serial);
	return 0;
}

/**
 * Backup structure descriptor for table-driven backup
 */
struct backup_structure_desc {
	const char		*name;
	uint64_t		pba_start;
	uint64_t		n_blocks;
	BOOL			is_first;		// true for first entry (no leading comma in JSON)
};

/**
 * Backup all NVMesh structures using table-driven approach.
 * Includes: 10 base structures + N segment metadata control blocks (variable)
 * Returns 0 on success, -1 on error.
 */
static int backup_all_structures(int disk_fd, const char *backup_dir, int pblk_size,
								 struct nvmeibt_disk_gpt *main_gpt,
								 struct nvmeibt_disk_gpt *metadata_gpt,
								 const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition,
								 struct nvmeibt_Str *manifest_json,
								 uint64_t *total_backup_bytes)
{
	uint64_t								n_entries_blocks_main;
	uint64_t								n_entries_blocks_metadata;
	struct backup_structure_desc			structures[10];

	// Calculate entry block counts
	n_entries_blocks_main = divroundup(main_gpt->header.n_partition_entries * main_gpt->header.size_of_partition_entry, pblk_size);
	n_entries_blocks_metadata = divroundup(metadata_gpt->header.n_partition_entries * metadata_gpt->header.size_of_partition_entry, pblk_size);

	// Define all 10 structures to backup (order matches restore expectations)
	structures[0] = (struct backup_structure_desc){"mbr",                        0,                                                      1,                          true};
	structures[1] = (struct backup_structure_desc){"main_gpt_primary_hdr",       main_gpt->header.my_pba,                                1,                          false};
	structures[2] = (struct backup_structure_desc){"main_gpt_primary_ent",       main_gpt->header.partition_entry_pba,                   n_entries_blocks_main,      false};
	structures[3] = (struct backup_structure_desc){"main_gpt_alternate_ent",     main_gpt->header.alternate_pba - n_entries_blocks_main, n_entries_blocks_main,      false};
	structures[4] = (struct backup_structure_desc){"main_gpt_alternate_hdr",     main_gpt->header.alternate_pba,                         1,                          false};
	structures[5] = (struct backup_structure_desc){"metadata_gpt_primary_hdr",   metadata_gpt->header.my_pba,                            1,                          false};
	structures[6] = (struct backup_structure_desc){"metadata_gpt_primary_ent",   metadata_gpt->header.partition_entry_pba,               n_entries_blocks_metadata,  false};
	structures[7] = (struct backup_structure_desc){"metadata_gpt_alternate_ent", metadata_gpt->header.alternate_pba - n_entries_blocks_metadata, n_entries_blocks_metadata, false};
	structures[8] = (struct backup_structure_desc){"metadata_gpt_alternate_hdr", metadata_gpt->header.alternate_pba,                     1,                          false};
	structures[9] = (struct backup_structure_desc){"disk_metadata",              disk_md_partition->pba_s,                               1,                          false};

	// Execute backups for fixed structures
	for (int i = 0; i < 10; i++) {
		if (backup_structure_and_append_manifest(disk_fd, structures[i].name, backup_dir,
												 structures[i].pba_start, structures[i].n_blocks, pblk_size,
												 manifest_json, total_backup_bytes, structures[i].is_first) < 0) {
			return -1;
		}
	}

	// Backup segment metadata control blocks (variable count)
	for (int i = 0; i < metadata_gpt->max_n_entries; i++) {
		const struct nvmeibt_disk_gpt_partition_entry	*seg_md_entry = &metadata_gpt->entries[i];
		char											seg_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];
		char											structure_name[128];

		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(seg_md_entry)) {
			continue;
		}
		if (!ARE_UUID_EQ(&seg_md_entry->partition_type_guid, &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID)) {
			continue;
		}

		/* Found segment metadata partition - backup its control block (first block) */
		char16_str_to_str(seg_md_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, seg_name);
		snprintf(structure_name, sizeof(structure_name), "seg_md_ctrl_%s", seg_name);

		if (backup_structure_and_append_manifest(disk_fd, structure_name, backup_dir,
												 seg_md_entry->pba_s, 1, pblk_size,
												 manifest_json, total_backup_bytes, false) < 0) {
			N_Wf(backup_seg_md_ctrl_failed, "Failed to backup segment metadata control block: @STR", seg_name);
			/* Continue with other partitions even if one fails */
		}
	}

	return 0;
}

/**
 * Create modular binary backup of critical disk structures before write
 * Creates private directory with manifest + structure files
 * Returns 0 on success, -1 on error
 * Backup format:
 *   - directory: GPT_UTIL_BACKUP_DIR/backup_<device>_<timestamp>/ (0700 permissions)
 *   - manifest: <directory>/manifest.json (0600 permissions)
 *   - structure files: <directory>/<structure>.bin (0600 permissions, 10 base + N segment control blocks)
 * NVMesh-only: REQUIRES Main GPT + Metadata GPT + disk_metadata readable
 */
static int create_binary_backup(int disk_fd, struct gpt_util_config *config, char *backup_prefix, size_t backup_prefix_size)
{
	time_t										now;
	struct tm									*tm_info;
	char										timestamp[64];
	char										device_basename[64];
	char										*last_slash;
	char										backup_dir[512];
	char										manifest_file[600];
	int											manifest_fd = -1;
	struct nvmeibt_Str							*manifest_json = NULL;
	struct nvmeibt_disk_gpt						*main_gpt = NULL;
	struct nvmeibt_disk_gpt						*metadata_gpt = NULL;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_md_partition = NULL;
	int											rv = -1;
	uint64_t									total_backup_bytes = 0;
	char										controller_serial_num[64] = {0};

	/* Generate backup prefix: GPT_UTIL_BACKUP_DIR/backup_<device>_<timestamp> */
	time(&now);
	tm_info = gmtime(&now);
	strftime(timestamp, sizeof(timestamp), "%m%d%Y_UTC%H%M%S", tm_info);

	/* Extract device name from path */
	last_slash = strrchr(config->device_path, '/');
	if (last_slash) {
		nvmeibt_strlcpy(device_basename, last_slash + 1, sizeof(device_basename));
	} else {
		nvmeibt_strlcpy(device_basename, config->device_path, sizeof(device_basename));
	}

	/* Create private backup directory: GPT_UTIL_BACKUP_DIR/backup_<device>_<timestamp>/ */
	snprintf(backup_dir, sizeof(backup_dir), GPT_UTIL_BACKUP_DIR "/backup_%s_%s", device_basename, timestamp);
	snprintf(backup_prefix, backup_prefix_size, "%s", backup_dir);
	snprintf(manifest_file, sizeof(manifest_file), "%s/manifest.json", backup_dir);

	N_IMf(backup_create_start, "Creating modular binary backup: dev=@STR dir=@STR",
		  config->device_path, backup_dir);

	/* Create backup directory with restrictive permissions (0700 - owner only) */
	if (create_backup_directory(backup_dir) < 0) {
		goto out;
	}

	main_gpt = NNVMEIBT_BM_CALLOC(trace_backup_main_gpt, sizeof(*main_gpt));
	if (!main_gpt) {
		N_Ef(backup_alloc_main_gpt_failed, "Failed to allocate main_gpt");
		goto out;
	}

	metadata_gpt = NNVMEIBT_BM_CALLOC(trace_backup_metadata_gpt, sizeof(*metadata_gpt));
	if (!metadata_gpt) {
		N_Ef(backup_alloc_metadata_gpt_failed, "Failed to allocate metadata_gpt");
		goto out;
	}

	manifest_json = NNVMEIBT_STR_ALLOC(trace_backup_manifest);

	/* Start JSON manifest */
	nvmeibt_Str_sprintf(manifest_json, "{\n");
	nvmeibt_Str_sprintf(manifest_json, "  \"gpt_util_version\": \"%s\",\n", GPT_UTIL_VERSION);
	nvmeibt_Str_sprintf(manifest_json, "  \"backup_timestamp\": \"%s\",\n", timestamp);
	nvmeibt_Str_sprintf(manifest_json, "  \"device_path\": \"%s\",\n", config->device_path);
	nvmeibt_Str_sprintf(manifest_json, "  \"block_size\": %d,\n", config->pblk_size);

	/* Read Main GPT to get structure locations */
	memset(main_gpt, 0, sizeof(*main_gpt));
	nvmeibt_strlcpy(main_gpt->main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt->main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, main_gpt,
										  config->pba_s, config->pba_hw_e, false) < 0) {
		N_Ef(backup_read_main_gpt_failed, "Failed to read Main GPT for backup");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read Main GPT - device not NVMesh formatted" COL_RESET "\n");
		goto out;
	}

	/* Validate device has Metadata GPT (required for all NVMesh devices) */
	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(main_gpt);
	if (!metadata_partition) {
		N_Ef(backup_no_metadata_partition, "Device has no EXCELERO_METADATA partition");
		fprintf(stderr, COL_RED_BOLD "ERROR: Device not NVMesh formatted (no metadata partition)" COL_RESET "\n");
		goto out;
	}

	memset(metadata_gpt, 0, sizeof(*metadata_gpt));
	nvmeibt_strlcpy(metadata_gpt->main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt->main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, metadata_gpt,
										  metadata_partition->pba_s, metadata_partition->pba_e, false) < 0) {
		N_Ef(backup_read_metadata_gpt_failed, "Failed to read Metadata GPT");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read Metadata GPT - device corrupted or not NVMesh formatted" COL_RESET "\n");
		goto out;
	}

	/* Require disk_metadata partition (validates device is NVMesh formatted) */
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(metadata_gpt);
	if (!disk_md_partition) {
		N_Ef(backup_no_disk_md_partition, "Device has no disk_metadata partition");
		fprintf(stderr, COL_RED_BOLD "ERROR: Device not NVMesh formatted (no disk_metadata partition)" COL_RESET "\n");
		goto out;
	}

	/* Get device serial number for restore validation */
	if (get_device_serial_num(disk_fd, config, controller_serial_num, sizeof(controller_serial_num)) < 0) {
		N_Ef(backup_get_serial_failed, "Failed to get device serial number");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read device serial number - blocking backup for safety" COL_RESET "\n");
		goto out;
	}

	/* Store controller serial number in manifest for restore validation */
	nvmeibt_Str_sprintf(manifest_json, "  \"controller_serial_num\": \"%s\",\n", controller_serial_num);
	nvmeibt_Str_sprintf(manifest_json, "  \"structures\": [\n");

	/* Backup all NVMesh structures (10 base + N segment control blocks) */
	if (backup_all_structures(disk_fd, backup_dir, config->pblk_size,
							  main_gpt, metadata_gpt, disk_md_partition,
							  manifest_json, &total_backup_bytes) < 0) {
		goto out;
	}

	/* Close JSON manifest */
	nvmeibt_Str_sprintf(manifest_json, "\n  ]\n");
	nvmeibt_Str_sprintf(manifest_json, "}\n");

	/* Write manifest file */
	/* O_EXCL | O_NOFOLLOW | 0600: Security hardening (prevent symlink attacks, owner-only access) */
	manifest_fd = open(manifest_file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (manifest_fd < 0) {
		N_Ef(backup_manifest_open_failed, "Failed to create manifest @STR @AUTO_ERRNO", manifest_file);
		goto out;
	}

	if (write(manifest_fd, nvmeibt_Str_str(manifest_json), nvmeibt_Str_strlen(manifest_json)) != (ssize_t)nvmeibt_Str_strlen(manifest_json)) {
		N_Ef(backup_manifest_write_failed, "Failed to write manifest @STR @AUTO_ERRNO", manifest_file);
		goto out;
	}

	/* fsync ensures manifest durability before returning success */
	if (fsync(manifest_fd) < 0) {
		N_Ef(backup_manifest_fsync_failed, "Failed to sync manifest @STR @AUTO_ERRNO", manifest_file);
		goto out;
	}

	N_IMf(backup_create_success, "Modular backup created: dir=@STR total_bytes=@SIZE_T", backup_dir, total_backup_bytes);
	fprintf(stdout, COL_GREEN "Modular backup created: %s (%lu bytes total)" COL_RESET "\n", backup_dir, total_backup_bytes);

	rv = 0;

out:
	if (manifest_fd >= 0) {
		close(manifest_fd);
	}
	NNVMEIBT_STR_FREE(trace_backup_manifest_free, manifest_json);
	NNVMEIBT_BM_FREE(trace_backup_main_gpt_free, main_gpt);
	NNVMEIBT_BM_FREE(trace_backup_metadata_gpt_free, metadata_gpt);
	return rv;
}

/**
 * Validates safety conditions (TOMA running) and prompts user for confirmation before write operations
 * Returns true if safe to write, and user confirms or skip_confirmation is set, false otherwise
 * BLOCKS if TOMA is running (safety check)
 */
static BOOL validate_and_confirm_write(struct gpt_util_config *config, const char *operation_description)
{
	char response[10];

	/* Safety check: Block writes if TOMA is running and managing device */
	if (is_toma_running_and_acquire_lock()) {
		N_Ef(confirm_toma_running, "TOMA is running - cannot write to device dev=@STR", config->device_path);
		fprintf(stderr, COL_RED_BOLD "\nERROR: TOMA is currently running!" COL_RESET "\n");
		fprintf(stderr, "Cannot modify GPT while TOMA is managing devices.\n");
		fprintf(stderr, "\n");
		fprintf(stderr, COL_YELLOW "To fix GPT, follow this procedure:" COL_RESET "\n");
		fprintf(stderr, "  1. Exclude device: Add to /etc/nvmesh/target_devices.conf\n");
		fprintf(stderr, "  2. Signal TOMA: pkill -1 nvmeibt_toma\n");
		fprintf(stderr, "  3. Find PCI address of this device (nvme10xxn1): ls /sys/bus/pci/drivers/nvmeibs/0000:*/misc; PCI_ADDR=\"0000:44:00.0\"\n");
		fprintf(stderr, "  4. Unbind from nvmeibs: echo <PCI_ADDR> > /sys/bus/pci/drivers/nvmeibs/unbind\n");
		fprintf(stderr, "  5. Run gpt_util to fix GPT\n");
		fprintf(stderr, "  6. Remove exclusion and signal TOMA again (undo step 1 and redo step 2)\n");
		fprintf(stderr, "  7. Rebind to nvmeibs: echo <PCI_ADDR> > /sys/bus/pci/drivers/nvme/bind\n");
		fprintf(stderr, "  8. Wait for rebuild to finish\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "Operation blocked for safety.\n");
		return false;
	}

	if (config->skip_confirmation) {
		return true;		// --yes flag: auto-confirm
	}

	fprintf(stdout, "\n" COL_YELLOW "WARNING: This operation will modify the disk!" COL_RESET "\n");
	fprintf(stdout, "Operation: %s\n", operation_description);
	fprintf(stdout, "Device: %s\n", config->device_path);
	fprintf(stdout, "\nProceed? [y/N]: ");
	fflush(stdout);

	if (fgets(response, sizeof(response), stdin) == NULL) {
		return false;		// EOF or error
	}

	if (response[0] == 'y' || response[0] == 'Y') {
		return true;
	}

	fprintf(stdout, "Operation cancelled by user.\n");
	return false;
}

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
	case ACTION_RESTORE_BINARY:
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
	const char *action_color;
	const char *action_prefix;

	if (old_entry) {
		char16_str_to_str(old_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, old_name);
	}
	if (new_entry) {
		char16_str_to_str(new_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, new_name);
	}


	/* Git-like colors: green for ADD, red for DELETE, yellow for MODIFY */
	if (old_entry && new_entry) {
		action_color = COL_YELLOW;
		action_prefix = "~";
	} else if (new_entry) {
		action_color = COL_GREEN;
		action_prefix = "+";
	} else {
		action_color = COL_RED;
		action_prefix = "-";
	}

	fprintf(stdout, "\n  %s%s %s Entry %d:" COL_RESET "\n", action_color, action_prefix, change_type, index);

	if (old_entry && new_entry) {
		// Modified
		if (strcmp(old_name, new_name) != 0) {
			fprintf(stdout, "    - Name: %s -> %s\n", old_name, new_name);
		}
		if (old_entry->pba_s != new_entry->pba_s || old_entry->pba_e != new_entry->pba_e) {
			fprintf(stdout, "    - Range: %lu-%lu -> %lu-%lu\n",
					old_entry->pba_s, old_entry->pba_e,
					new_entry->pba_s, new_entry->pba_e);
		}
		if (old_entry->attributes != new_entry->attributes) {
			fprintf(stdout, "    - Attributes: 0x%lx -> 0x%lx\n",
					old_entry->attributes, new_entry->attributes);
		}
	} else if (new_entry) {
		// Added
		type_uuid = nvmeibt_union_uuid_to_urn_uuid(&new_entry->partition_type_guid);
		fprintf(stdout, "    - Name: %s\n", new_name);
		fprintf(stdout, "    - Type: %s\n", type_uuid.str);
		fprintf(stdout, "    - Range: %lu-%lu\n", new_entry->pba_s, new_entry->pba_e);
	} else {
		// Deleted
		type_uuid = nvmeibt_union_uuid_to_urn_uuid(&old_entry->partition_type_guid);
		fprintf(stdout, "    - Name: %s\n", old_name);
		fprintf(stdout, "    - Type: %s\n", type_uuid.str);
		fprintf(stdout, "    - Range: %lu-%lu\n", old_entry->pba_s, old_entry->pba_e);

		// Warn if this is a critical partition
		if (ARE_UUID_EQ(&old_entry->partition_type_guid, &EXCELERO_METADATA_PARTITION_TYPE_GUID) ||
			ARE_UUID_EQ(&old_entry->partition_type_guid, &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID)) {
			N_Wf(delete_critical_partition, "Deleting critical partition: name=@STR type=@UUID_LE",
				 old_name, &old_entry->partition_type_guid);
			fprintf(stdout, "    " COL_RED_BOLD "[WARNING] This is a critical NVMesh partition!" COL_RESET "\n");
			fprintf(stdout, "    " COL_YELLOW "          Deletion will make device unusable by NVMesh." COL_RESET "\n");
		}
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
	int		n_overlaps = 0;

	for (int i = 0; i < max_n_entries; i++) {
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(&entries[i])) {
			continue;
		}

		for (int j = i + 1; j < max_n_entries; j++) {
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

		for (int i = 0; i < max_n_entries; i++) {
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
			for (int i = 0; i < max_n_entries; i++) {
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

	// Find metadata partition for nested GPT display
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
	int						entry_count = 0;

	// Build section name from enums using helper functions
	nvmeibt_Str_sprintf(json_output, "  \"%s_gpt_%s\": {\n",
						gpt_level_str(level), gpt_copy_str(copy));

	// Editable fields
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&header->disk_obj_uuid);
	nvmeibt_Str_sprintf(json_output, "    \"disk_uuid\": \"%s\",\n", urn_uuid.str);
	nvmeibt_Str_sprintf(json_output, "    \"first_usable_pba\": %lu,\n", header->first_usable_pba);
	nvmeibt_Str_sprintf(json_output, "    \"last_usable_pba\": %lu,\n", header->last_usable_pba);

	// Static fields (UEFI constants - do not edit)
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_gpt_signature\": \"0x%lx\",\n", header->gpt_signature);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_revision\": \"0x%08x\",\n", header->revision);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_header_size\": %d,\n", header->header_size);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_size_of_partition_entry\": %d,\n", header->size_of_partition_entry);

	// Readonly fields (recalculated on write - do not edit)
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_n_partition_entries\": %d,\n", header->n_partition_entries);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_header_crc32\": \"0x%08x\",\n", header->header_crc32);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_partition_entry_array_crc32\": \"0x%08x\",\n", header->partition_entry_array_crc32);

	nvmeibt_Str_sprintf(json_output, "    \"entries\": [\n");

	// Export partition entries
	for (int i = 0; i < max_n_entries; i++) {
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
			nvmeibt_Str_sprintf(json_output, "        \"name\": \"%s\",\n", partition_name_str);
			nvmeibt_Str_sprintf(json_output, "        \"_delete\": false\n");
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
	struct nvmeibt_disk_gpt					main_gpt;
	struct nvmeibt_disk_gpt					metadata_gpt;
	struct nvmeibt_disk_mbr					mbr;
	struct gpt_buffers						bufs;
	struct gpt_buffers						metadata_bufs = {0};
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_metadata_partition = NULL;
	struct nvmeibt_disk_metadata			*disk_md = NULL;
	struct nvmeibt_urn_uuid					mgmt_uuid_urn;
	struct nvmeibt_urn_uuid					nguid_urn;
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
	uint64_t								pbyte_s;
	BOOL									is_mismatch = false;
	BOOL									has_overlaps = false;
	char									controller_serial_num[64] = {0};
	int										seg_md_count = 0;
	struct nvmeibt_seg_active_metadata_ctrl	*seg_md_ctrl = NULL;

	json_output = NNVMEIBT_STR_ALLOC(trace_gpt_json_export);

	// Allocate buffers
	alloc_gpt_buffers(&bufs, config->pblk_size, LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	memset(&main_gpt, 0, sizeof(main_gpt));
	main_gpt.max_n_entries = LARGE_GPT_MAX_NUM_GPT_ENTRIES;
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));

	// Read all 4 Main GPT structures
	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
		NULL, disk_fd, config->pblk_size, &main_gpt,
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
		has_overlaps = (detect_overlaps(bufs.primary_entries, main_gpt.max_n_entries) > 0);
	}
	if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		has_overlaps = has_overlaps || (detect_overlaps(bufs.alternate_entries, main_gpt.max_n_entries) > 0);
	}

	// Get timestamp
	time(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

	// Read pMBR
	memset(&mbr, 0, sizeof(mbr));
	nvmeibt_disk_metadata_read_mbr_blk(NULL, disk_fd, config->pblk_size, &mbr, config->device_path, NULL);

	// Get device serial number for validation
	if (get_device_serial_num(disk_fd, config, controller_serial_num, sizeof(controller_serial_num)) < 0) {
		N_Ef(export_get_serial_failed, "Failed to get device serial number");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read device serial number - export blocked" COL_RESET "\n");
		goto out;
	}

	// Start JSON with metadata
	nvmeibt_Str_sprintf(json_output, "{\n");
	nvmeibt_Str_sprintf(json_output, "  \"gpt_util_version\": \"%s\",\n", GPT_UTIL_VERSION);
	nvmeibt_Str_sprintf(json_output, "  \"backup_timestamp\": \"%s\",\n", timestamp);
	nvmeibt_Str_sprintf(json_output, "  \"device_path\": \"%s\",\n", config->device_path);
	nvmeibt_Str_sprintf(json_output, "  \"=== SECTION 1 ===\": \"AUTO-DETECTED STATUS - DO NOT EDIT\",\n");
	nvmeibt_Str_sprintf(json_output, "  \"_READONLY_controller_serial_num\": \"%s\",\n", controller_serial_num);
	nvmeibt_Str_sprintf(json_output, "  \"_READONLY_mismatch_detected\": %s,\n", is_mismatch ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "  \"_READONLY_overlaps_detected\": %s,\n", has_overlaps ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "  \"=== SECTION 2 ===\": \"DISK STRUCTURE DATA - EDIT WITH CAUTION\",\n");

	// NVMesh-only: REQUIRE metadata GPT exists and is readable
	memset(&main_gpt, 0, sizeof(main_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, disk_fd, config->pblk_size, &main_gpt,
										  config->pba_s, config->pba_hw_e, false) < 0) {
		N_Ef(export_read_main_gpt_failed, "Failed to read Main GPT for export");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read Main GPT - device not NVMesh formatted" COL_RESET "\n");
		goto out;
	}

	metadata_partition = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&main_gpt);
	if (!metadata_partition) {
		N_Ef(export_no_metadata_partition, "Device has no EXCELERO_METADATA partition");
		fprintf(stderr, COL_RED_BOLD "ERROR: Device not NVMesh formatted (no metadata partition)" COL_RESET "\n");
		fprintf(stderr, "  gpt_util only supports NVMesh devices.\n");
		goto out;
	}

	// Export pMBR
	nvmeibt_Str_sprintf(json_output, "  \"pmbr\": {\n");
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_signature\": \"0x%04x\",\n", (unsigned short)mbr.signature);
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_os_type\": \"0x%02x\",\n", (unsigned char)mbr.partitions[0].os_type);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_pba_s\": %d,\n", mbr.partitions[0].pba_s);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_n_pblk\": %d\n", mbr.partitions[0].n_pblk);
	nvmeibt_Str_sprintf(json_output, "  }");

	// Export Main GPT based on --gpt-copy option
	nvmeibt_Str_sprintf(json_output, ",\n");
	if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
		/* Metadata GPT always exists (NVMesh-only), so never last */
		export_gpt_copy_entries_to_json(GPT_LEVEL_MAIN, GPT_COPY_PRIMARY,
										bufs.primary_header, bufs.primary_entries,
										main_gpt.max_n_entries, json_output, false);
	}
	if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		/* Metadata GPT always exists (NVMesh-only), so never last */
		export_gpt_copy_entries_to_json(GPT_LEVEL_MAIN, GPT_COPY_ALTERNATE,
										bufs.alternate_header, bufs.alternate_entries,
										main_gpt.max_n_entries, json_output, false);
	}

	// Export Metadata GPT (always exists for NVMesh devices)
	alloc_gpt_buffers(&metadata_bufs, config->pblk_size, MAX_NUM_GPT_ENTRIES);
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	metadata_gpt.max_n_entries = MAX_NUM_GPT_ENTRIES;
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));

	// Read all 4 Metadata GPT structures to metadata_bufs
	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
		NULL, disk_fd, config->pblk_size, &metadata_gpt,
		metadata_partition->pba_s, metadata_partition->pba_e,
		&meta_primary_header_validity, &meta_alternate_header_validity,
		&meta_primary_entries_validity, &meta_alternate_entries_validity,
		metadata_bufs.primary_header, metadata_bufs.alternate_header,
		metadata_bufs.primary_entries, metadata_bufs.alternate_entries);

	// NVMesh-only: REQUIRE EXCELERO_DISK_METADATA partition
	for (int k = 0; k < metadata_gpt.max_n_entries; k++) {
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&metadata_bufs.primary_entries[k])) {
			if (ARE_UUID_EQ(&metadata_bufs.primary_entries[k].partition_type_guid,
							&EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID)) {
				disk_metadata_partition = &metadata_bufs.primary_entries[k];
				break;
			}
		}
	}

	if (!disk_metadata_partition) {
		N_Ef(export_no_disk_md_partition, "Device has no disk_metadata partition");
		fprintf(stderr, COL_RED_BOLD "ERROR: Device not NVMesh formatted (no disk_metadata partition)" COL_RESET "\n");
		goto out;
	}

	pbyte_s = disk_metadata_partition->pba_s * config->pblk_size;
	disk_md = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_export_disk_md, PAGE_SIZE, sizeof(*disk_md));
	if (!disk_md || nvmeibt_disk_metadata_read_disk_metadata(NULL, disk_fd, config->pblk_size,
												 pbyte_s, disk_md) < 0) {
		N_Ef(export_read_disk_md_failed, "Failed to read disk_metadata");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot read disk_metadata - device corrupted" COL_RESET "\n");
		goto out;
	}

	// Export Metadata GPT (disk_metadata always last)
	if (config->gpt_copy_option & GPT_COPY_OPTION_PRIMARY) {
		export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_PRIMARY,
										metadata_bufs.primary_header,
										metadata_bufs.primary_entries,
										metadata_gpt.max_n_entries, json_output, false);
	}
	if (config->gpt_copy_option & GPT_COPY_OPTION_ALTERNATE) {
		export_gpt_copy_entries_to_json(GPT_LEVEL_METADATA, GPT_COPY_ALTERNATE,
										metadata_bufs.alternate_header,
										metadata_bufs.alternate_entries,
										metadata_gpt.max_n_entries, json_output, false);
	}

	// Export disk_metadata
	N_Tf(gpt_export_disk_md, "Exporting disk_metadata structure");

	nvmeibt_Str_sprintf(json_output, "  \"disk_metadata\": {\n");

	/* Static fields (constants - do not edit) */
	nvmeibt_Str_sprintf(json_output, "    \"_STATIC_signature\": \"0x%lx\",\n", disk_md->signature);

	/* Readonly fields (from hardware - do not edit) */
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_nsid\": %d,\n", disk_md->nsid);
	nvmeibt_Str_sprintf(json_output, "    \"_READONLY_crc32\": \"0x%08x\",\n", disk_md->crc32);

	/* Editable fields (safe configuration) */
	nvmeibt_Str_sprintf(json_output, "    \"native_serial_str\": \"%s\",\n", disk_md->native_serial_str);

	/* Editable fields (safe configuration) */
	mgmt_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&disk_md->mgmt_db_uuid);
	nvmeibt_Str_sprintf(json_output, "    \"mgmt_db_uuid\": \"%s\",\n", mgmt_uuid_urn.str);
	nvmeibt_Str_sprintf(json_output, "    \"disk_metadata_version\": %u,\n", disk_md->disk_metadata_version);
	nvmeibt_Str_sprintf(json_output, "    \"format_pblk_size\": %u,\n", disk_md->format_pblk_size);
	nvmeibt_Str_sprintf(json_output, "    \"format_metadata_size\": %u,\n", disk_md->format_metadata_size);
	nvmeibt_Str_sprintf(json_output, "    \"is_md_supported\": %s,\n", disk_md->is_md_supported ? "true" : "false");
	nvmeibt_Str_sprintf(json_output, "    \"ldisk_id_str\": \"%s\",\n", disk_md->ldisk_id_str);
	nguid_urn = nvmeibt_union_uuid_to_urn_uuid(&disk_md->native_nguid_unused);
	nvmeibt_Str_sprintf(json_output, "    \"native_nguid\": \"%s\",\n", nguid_urn.str);

	/* Editable with WARNING (system state - dangerous!) */
	nvmeibt_Str_sprintf(json_output, "    \"_WARNING_last_pba_zeroed\": %lu,\n", disk_md->last_pba_zeroed);
	nvmeibt_Str_sprintf(json_output, "    \"_WARNING_format_request_counter\": %u\n", disk_md->format_request_counter);

	nvmeibt_Str_sprintf(json_output, "  },\n");

	NNVMEIBT_BM_FREE(trace_gpt_export_disk_md_free, disk_md);

	// Export segment_metadata control blocks (first 4K of each segment metadata partition)
	N_Tf(gpt_export_seg_md, "Exporting segment_metadata control blocks");

	nvmeibt_Str_sprintf(json_output, "  \"segment_metadata_partitions\": [\n");

	seg_md_ctrl = NNVMEIBT_BM_ALIGNED_CALLOC(trace_gpt_export_seg_md, PAGE_SIZE, sizeof(*seg_md_ctrl));
	if (!seg_md_ctrl) {
		N_Ef(export_alloc_seg_md_failed, "Failed to allocate segment metadata control block");
		fprintf(stderr, COL_RED_BOLD "ERROR: Cannot allocate memory for segment metadata export" COL_RESET "\n");
		goto out;
	}

	for (int k = 0; k < metadata_gpt.max_n_entries; k++) {
		const struct nvmeibt_disk_gpt_partition_entry	*seg_md_entry = &metadata_bufs.primary_entries[k];
		struct nvmeibt_urn_uuid							seg_uuid_urn;
		struct nvmeibt_urn_uuid							seg_mgmt_uuid_urn;
		char											seg_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];
		uint64_t										seg_md_pbyte_s;

		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(seg_md_entry)) {
			continue;
		}

		if (!ARE_UUID_EQ(&seg_md_entry->partition_type_guid, &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID)) {
			continue;
		}

		/* Read the first 4K block (segment metadata control structure) */
		seg_md_pbyte_s = seg_md_entry->pba_s * config->pblk_size;
		if (nvmeibt_ds_metadata_ctrl_blk_read(NULL, disk_fd, config->pblk_size, seg_md_pbyte_s, seg_md_ctrl) < 0) {
			N_Wf(export_seg_md_read_failed, "Failed to read segment metadata control block at pba=@LLU", seg_md_entry->pba_s);
			continue;
		}

		// Convert partition name and UUIDs
		char16_str_to_str(seg_md_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, seg_name);
		seg_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&seg_md_ctrl->disk_segment_uuid);
		seg_mgmt_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&seg_md_ctrl->header.mgmt_db_uuid);

		// Add comma if not first entry
		if (seg_md_count > 0) {
			nvmeibt_Str_sprintf(json_output, ",\n");
		}

		nvmeibt_Str_sprintf(json_output, "    {\n");
		/* Static fields (constants - do not edit) */
		nvmeibt_Str_sprintf(json_output, "      \"_STATIC_magic_str\": \"%s\",\n", seg_md_ctrl->header.magic_str);

		/* Readonly fields (from hardware - do not edit) */
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_partition_name\": \"%s\",\n", seg_name);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_partition_pba_s\": %llu,\n", seg_md_entry->pba_s);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_partition_pba_e\": %llu,\n", seg_md_entry->pba_e);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_software_version\": %u,\n", seg_md_ctrl->header.software_version);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_seg_metadata_version\": %u,\n", seg_md_ctrl->header.seg_metadata_version);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_save_timespec_tv_sec\": %lld,\n", (long long)seg_md_ctrl->save_timespec_tv_sec);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_locks_table_pbyte_s\": %llu,\n", seg_md_ctrl->locks_table_pbyte_s);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_committed_praid_config_version\": %d,\n", seg_md_ctrl->committed_praid_config_version);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_is_written_on_disk\": %d,\n", seg_md_ctrl->is_written_on_disk);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_metadata_pbyte_s\": %llu,\n", seg_md_ctrl->metadata_pbyte_s);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_metadata_ctrl_crc32\": \"0x%08x\",\n", seg_md_ctrl->metadata_ctrl_crc32);
		nvmeibt_Str_sprintf(json_output, "      \"_READONLY_locks_table_crc32\": \"0x%08x\",\n", seg_md_ctrl->locks_table_crc32);

		/* Editable fields (safe configuration) */
		nvmeibt_Str_sprintf(json_output, "      \"mgmt_db_uuid\": \"%s\",\n", seg_mgmt_uuid_urn.str);
		nvmeibt_Str_sprintf(json_output, "      \"hostname\": \"%s\",\n", seg_md_ctrl->hostname);

		/* Editable with WARNING (system state - dangerous!) */
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_disk_segment_uuid\": \"%s\",\n", seg_uuid_urn.str);
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_reservation_mode_version\": %llu,\n", seg_md_ctrl->reservation_mode_version);
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_active_praid_version_major\": %d,\n", seg_md_ctrl->active_praid_version_major);
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_active_praid_version_minor\": %d,\n", seg_md_ctrl->active_praid_version_minor);
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_is_current_shutdown_clean\": %s,\n", seg_md_ctrl->is_current_shutdown_clean ? "true" : "false");
		nvmeibt_Str_sprintf(json_output, "      \"_WARNING_n_blksets_scrubbed\": %llu\n", seg_md_ctrl->n_blksets_scrubbed);
		nvmeibt_Str_sprintf(json_output, "    }");

		seg_md_count++;
	}

	NNVMEIBT_BM_FREE(trace_gpt_export_seg_md_free, seg_md_ctrl);
	seg_md_ctrl = NULL;
	nvmeibt_Str_sprintf(json_output, "\n  ],\n");

	/* Export in-memory GPT from TOMA if running */
	nvmeibt_Str_sprintf(json_output, "  \"=== SECTION 3 ===\": \"TOMA IN-MEMORY GPT - READ-ONLY\",\n");
	if (is_toma_running_and_acquire_lock()) { // TOMA is running
		struct nvmeibt_Str *memory_gpt_json = NNVMEIBT_STR_ALLOC(trace_gpt_export_memory_gpt);

		nvmeibt_Str_sprintf(json_output, "  \"_toma_running\": true");

		if (get_memory_gpt_via_rpc(config->device_path, memory_gpt_json, config->is_self_test) == 0) {
			N_Tf(export_got_memory_gpt, "Retrieved in-memory GPT from TOMA for dev=@STR", config->device_path);
			nvmeibt_Str_sprintf(json_output, ",\n");
			nvmeibt_Str_sprintf(json_output, "  \"_memory_gpt_available\": true,\n");

			/* Merge memory GPT JSON fields directly */
			nvmeibt_Str_sprintf(json_output, "%s\n", nvmeibt_Str_str(memory_gpt_json));
		} else {
			N_Tf(export_memory_gpt_unavailable, "Could not retrieve in-memory GPT from TOMA dev=@STR", config->device_path);
			nvmeibt_Str_sprintf(json_output, ",\n");
			nvmeibt_Str_sprintf(json_output, "  \"_memory_gpt_available\": false\n");
		}

		NNVMEIBT_STR_FREE(trace_gpt_export_memory_gpt_free, memory_gpt_json);
	} else {
		// Lock remains held - will be released at end of gpt_util operation
		N_Tf(export_toma_not_running, "TOMA not running");
		nvmeibt_Str_sprintf(json_output, "  \"_toma_running\": false\n");
	}

	nvmeibt_Str_sprintf(json_output, "}\n");

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

	N_IMf(gpt_json_export_success, "GPT exported to JSON: dev=@STR file=@STR bytes=@SIZE_T copy_option=@STR",
		  config->device_path, output_file, nvmeibt_Str_strlen(json_output), gpt_copy_option_str(config->gpt_copy_option));
	fprintf(stdout, COL_GREEN "GPT exported to JSON: %s (%lu bytes)" COL_RESET "\n", output_file, nvmeibt_Str_strlen(json_output));

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
	free_gpt_buffers(&metadata_bufs);
	NNVMEIBT_BM_FREE(trace_gpt_export_seg_md_cleanup, seg_md_ctrl);
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
	fprintf(stdout, "  -i, --check-nvmesh          Check if NVMESH_METADATA partition exists\n");
	fprintf(stdout, "  -f, --fix-gpt               Fix GPT from alternate copy (and display)\n");
	fprintf(stdout, "  -F, --fix-mbr               Fix MBR (and display)\n");
	fprintf(stdout, "  -U, --upgrade-gpt           Fix n_partition_entries to 8192 and recalculate CRC\n");
	fprintf(stdout, "  -J, --output-json=FILE      Export GPT to JSON file\n");
	fprintf(stdout, "  -A, --apply-from=FILE       Apply GPT from JSON file (dry-run by default)\n");
	fprintf(stdout, "  -R, --restore-binary=FILE   Restore device from binary backup (dry-run by default)\n");
	fprintf(stdout, "  (default: display GPT)      Display GPT structure\n\n");

	fprintf(stdout, "Display Options:\n");
	fprintf(stdout, "  -c, --gpt-copy=WHICH        Which copy: primary|alternate|both (default: primary)\n");
	fprintf(stdout, "  -u, --filter-uuid=UUID      Show only entries matching UUID\n");
	fprintf(stdout, "  -l, --filter-lba=ADDR       Show only entries containing LBA address\n");
	fprintf(stdout, "  -Z, --print-zero-verify     Print commands that verify zeroed ranges\n\n");

	fprintf(stdout, "Apply Options:\n");
	fprintf(stdout, "  -W, --write                 Actually write changes (default: dry-run)\n");
	fprintf(stdout, "  -Y, --yes                   Skip confirmation prompt (auto-confirm writes)\n\n");

	fprintf(stdout, "I/O Options:\n");
	fprintf(stdout, "  -D, --direct                Force O_DIRECT even for regular files (may fail)\n");
	fprintf(stdout, "  -N, --no-direct             Disable O_DIRECT even for block devices\n");
	fprintf(stdout, "  (default: auto)             Block devices use O_DIRECT, files don't\n\n");

	fprintf(stdout, "Testing:\n");
	fprintf(stdout, "  -T, --self-test             Run comprehensive self-test suite\n");
	fprintf(stdout, "  -q, --quiet                 Quiet mode (suppress decorative banners in tests)\n\n");

	fprintf(stdout, "Help:\n");
	fprintf(stdout, "  -h, --help                  Display this help message\n\n");

	fprintf(stdout, COL_BLUE "Examples:" COL_RESET "\n\n");

	fprintf(stdout, "Display GPT:\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1\t\t\t\t# Display primary copy\n", argv[0]);
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 -c both\t\t\t# Display both primary and alternate\n", argv[0]);
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --filter-lba=1000\t\t# Filter by LBA address\n\n", argv[0]);

	fprintf(stdout, "Fix corrupted GPT:\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --fix-gpt\t\t\t# Fix from alternate copy\n", argv[0]);
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --fix-mbr\t\t\t# Fix MBR\n\n", argv[0]);

	fprintf(stdout, "Export/Apply Workflow (Edit GPT via JSON):\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --output-json=backup.json\t# Export to JSON\n", argv[0]);
	fprintf(stdout, "  vim backup.json\t\t\t\t\t\t# Edit (delete partition, etc)\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --apply-from=backup.json\t# Preview (dry-run)\n", argv[0]);
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --apply-from=backup.json --write\t# Apply changes\n\n", argv[0]);

	fprintf(stdout, "Binary Backup/Restore:\n");
	fprintf(stdout, "  # Automatic backup before writes:\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --apply-from=changes.json --write\n", argv[0]);
	fprintf(stdout, "  # Creates: " GPT_UTIL_BACKUP_DIR "/backup_nvme0n1_<timestamp>/\n");
	fprintf(stdout, "  \n");
	fprintf(stdout, "  # Manual restore:\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 --restore-binary=" GPT_UTIL_BACKUP_DIR "/backup_nvme0n1_<timestamp>/manifest.json\n\n", argv[0]);

	fprintf(stdout, "Advanced:\n");
	fprintf(stdout, "  %s -a " TOMA_ROOT_DIR "dev/nvme0n1 -Z\t\t\t\t# Print zeroing verification commands\n", argv[0]);
	fprintf(stdout, "  %s -T\t\t\t\t\t\t\t# Run all self-tests\n", argv[0]);
	fprintf(stdout, "  %s -T 1,5,10-15\t\t\t\t\t# Run specific tests\n\n", argv[0]);

	fprintf(stdout, COL_YELLOW "Note:\n");
	fprintf(stdout, "      1. gpt_util requires properly formatted NVMesh devices (Main GPT + Metadata GPT + disk_metadata must be readable)\n");
	fprintf(stdout, "      2. For read-only mode, it works when TOMA is up, but target device has to be unbound from nvmeibs.\n");
	fprintf(stdout, "      3. For write mode or self-test mode, TOMA has to be stopped.\n" COL_RESET);
}


/**
 * Helper macro: Check if action already set (only one action allowed)
 */
#define CHECK_SINGLE_ACTION(config, error_name) \
	do { \
		if ((config)->action != ACTION_DISPLAY_GPT) { \
			N_Ef(error_name, "Multiple actions specified (only one allowed)"); \
			rv = -1; \
			goto out; \
		} \
	} while(0)

/**
 * Phase 1: Parse command-line arguments into config structure
 * Returns 0 on success, -1 on error
 */
static int parse_arguments(int argc, char *argv[], struct gpt_util_config *config)
{
	int								rv = 0;
	int								op;
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
		{"upgrade-gpt",				no_argument,		0,	'U'},
		{"gpt-copy",				required_argument,	0,	'c'},
		{"filter-uuid",				required_argument,	0,	'u'},
		{"filter-lba",				required_argument,	0,	'l'},
		{"print-zero-verify",		no_argument,		0,	'Z'},
		{"output-json",				required_argument,	0,	'J'},
		{"apply-from",				required_argument,	0,	'A'},
		{"restore-binary",			required_argument,	0,	'R'},
		{"write",					no_argument,		0,	'W'},
		{"yes",						no_argument,		0,	'Y'},
		{"direct",					no_argument,		0,	'D'},
		{"no-direct",				no_argument,		0,	'N'},
		{"help",					no_argument,		0,	'h'},

		{0, 0, 0, 0}
	};
	static const char short_options[] = "d:a:s:e:b:c:u:l:J:A:R:ZimfFUWDNYh";
	static int long_idx = -1;

	for (int i = 0; i < argc; ++i) {
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
					scan_line_end = csv_str_end;
				}
				line_len = strnlen(scan_line_ptr, scan_line_end - scan_line_ptr);
				/* Advance pointer even if line is too long (prevents infinite loop) */
				if (line_len >= (int)sizeof(line)) {
					N_Wf(parse_csv_line_too_long, "CSV line too long (@INT bytes), skipping", line_len);
					scan_line_ptr = scan_line_end + 1;
					continue;
				}
				memcpy(line, scan_line_ptr, line_len);
				scan_line_ptr = scan_line_ptr + line_len + 1;
				line[line_len] = '\0';
				if (line_len < 1) {
					continue;
				}

				if (is_expecting_csv_header_line) {
					const char *ref_header = nvmeibt_get_csv_header_by_section_type(section_type);
					int ref_header_len;

					if (ref_header == NULL) {
						N_Ef(parse_csv_bad_section_type, "Wrong section_type=@INT", section_type);
						rv = -1;
						goto out;
					}

					ref_header_len = strlen(ref_header);
					if (line_len != ref_header_len || memcmp(line, ref_header, line_len) != 0) {
						N_Ef(parse_csv_header_mismatch, "CSV header mismatch: expected '@STR' (@INT bytes), got '@STR' (@INT bytes)",
							 ref_header, ref_header_len, line, line_len);
						rv = -1;
						goto out;
					}
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
						break;
					}
				}
			}
			if (config->pblk_size == 0) {
				N_Ef(parse_csv_device_not_found, "Device not found in disks CSV: @STR", config->device_path);
				fprintf(stderr, COL_RED_BOLD "ERROR: Device '%s' not found in disks.csv" COL_RESET "\n", config->device_path);
				rv = -1;
				goto out;
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
			CHECK_SINGLE_ACTION(config, parse_multiple_actions);
			config->action = ACTION_DISPLAY_MBR;
			fprintf(stdout, "Action: Display MBR\n");
			break;
		case 'f':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_fix_gpt);
			config->action = ACTION_FIX_GPT;
			fprintf(stdout, "Action: Fix GPT from alternate copy\n");
			break;
		case 'F':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_fix_mbr);
			config->action = ACTION_FIX_MBR;
			fprintf(stdout, "Action: Fix MBR\n");
			break;
		case 'i':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_check);
			config->action = ACTION_CHECK_NVMESH;
			fprintf(stdout, "Action: Check for NVMESH_METADATA partition\n");
			break;
		case 'U':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_upgrade);
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
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_export);
			config->action = ACTION_EXPORT_JSON;
			nvmeibt_strlcpy(config->output_json_file, optarg, sizeof(config->output_json_file));
			fprintf(stdout, "Action: Export GPT to JSON file: %s\n", config->output_json_file);
			break;
		case 'A':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_apply);
			config->action = ACTION_APPLY_JSON;
			nvmeibt_strlcpy(config->apply_json_file, optarg, sizeof(config->apply_json_file));
			fprintf(stdout, "Action: Apply GPT from JSON file: %s (dry-run by default)\n", config->apply_json_file);
			break;
		case 'R':
			CHECK_SINGLE_ACTION(config, parse_multiple_actions_restore);
			config->action = ACTION_RESTORE_BINARY;
			nvmeibt_strlcpy(config->restore_binary_file, optarg, sizeof(config->restore_binary_file));
			fprintf(stdout, "Action: Restore from binary backup: %s\n", config->restore_binary_file);
			break;
		case 'W':
			config->write_mode = true;
			fprintf(stdout, "Write mode: ENABLED (changes will be written to disk)\n");
			break;
		case 'Y':
			config->skip_confirmation = true;
			fprintf(stdout, "Confirmation: SKIPPED (--yes flag)\n");
			break;
		case 'D':
			config->o_direct_mode = O_DIRECT_FORCE_ON;
			fprintf(stdout, "I/O mode: Force O_DIRECT (may fail for regular files)\n");
			break;
		case 'N':
			config->o_direct_mode = O_DIRECT_FORCE_OFF;
			fprintf(stdout, "I/O mode: Disable O_DIRECT\n");
			break;
		case 'h':
			print_usage(argv);
			exit(0);
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
		char backup_path[512];

		fprintf(stdout, "MBR signature invalid, attempting fix...\n");
		if (config->pba_e == 0) {
			N_Ef(fix_mbr_no_size, "Disk size not detected, please specify with -e");
			goto out;
		}

		/* Confirm before write */
		if (!validate_and_confirm_write(config, "Fix MBR")) {
			fprintf(stdout, "MBR fix cancelled.\n");
			goto out;
		}

		/* Create binary backup before modifying */
		if (create_binary_backup(disk_fd, config, backup_path, sizeof(backup_path)) < 0) {
			fprintf(stderr, COL_RED_BOLD "ERROR: Backup failed - aborting write operation for safety" COL_RESET "\n");
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
	char	backup_path[512];

	/* Confirm before fix */
	if (!validate_and_confirm_write(config, "Fix GPT from alternate copy")) {
		fprintf(stdout, "GPT fix cancelled.\n");
		return -1;
	}

	/* Create binary backup */
	if (create_binary_backup(disk_fd, config, backup_path, sizeof(backup_path)) < 0) {
		fprintf(stderr, COL_RED_BOLD "ERROR: Backup failed - aborting write operation for safety" COL_RESET "\n");
		return -1;
	}

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
	char									backup_path[512];
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_entry;

	memset(&main_gpt, 0, sizeof(main_gpt));
	memset(&metadata_gpt, 0, sizeof(metadata_gpt));
	nvmeibt_strlcpy(main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(main_gpt.main_or_metadata));
	nvmeibt_strlcpy(metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(metadata_gpt.main_or_metadata));

	fprintf(stdout, "\n=== GPT Upgrade Check (n_partition_entries -> %d) ===\n\n", LARGE_GPT_MAX_NUM_GPT_ENTRIES);

	/* Confirm before upgrade */
	if (!validate_and_confirm_write(config, "Upgrade GPT (fix n_partition_entries to 8192)")) {
		fprintf(stdout, "GPT upgrade cancelled.\n");
		goto out;
	}

	/* Create binary backup */
	if (create_binary_backup(disk_fd, config, backup_path, sizeof(backup_path)) < 0) {
		fprintf(stderr, COL_RED_BOLD "ERROR: Backup failed - aborting write operation for safety" COL_RESET "\n");
		goto out;
	}

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
 * Returns: 0 = normal entry, 1 = entry marked for deletion (_delete: true), -1 = error
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
	BOOL						is_delete_requested = false;
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
		JSON_ASSIGN_OPTIONAL(parse_delete, "_delete");	// Optional, marks entry for deletion
		if (strcmp(kv->key, "_delete") == 0 && kv->value->type == JSON_E_BOOL && kv->value->num != 0) {
			is_delete_requested = true;
		}
		JSON_LOOP_ITERATION_END(parse_entry_end, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(parse_entry_validate);

	// If deletion requested, return special status (entry content doesn't matter)
	if (is_delete_requested) {
		return 1;		// Deletion requested
	}

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
		JSON_ASSIGN_PLAIN(parse_first_pba, "first_usable_pba", gpt->header.first_usable_pba, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_last_pba, "last_usable_pba", gpt->header.last_usable_pba, (uint64_t)kv->value->num);
		JSON_ASSIGN_PLAIN(parse_entries_arr, "entries", entries_array, kv->value);
		JSON_ASSIGN_OPTIONAL(parse_static_sig, "_STATIC_gpt_signature");
		JSON_ASSIGN_OPTIONAL(parse_static_rev, "_STATIC_revision");
		JSON_ASSIGN_OPTIONAL(parse_static_hdr_sz, "_STATIC_header_size");
		JSON_ASSIGN_OPTIONAL(parse_static_ent_sz, "_STATIC_size_of_partition_entry");
		JSON_ASSIGN_OPTIONAL(parse_ro_n_part, "_READONLY_n_partition_entries");
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

	for (int i = 0; i < n_entries_in_json; i++) {
		struct mm_json_elem						*entry_elem = entries_array->array.elements[i];
		struct mm_json_dict						*entry_dict = NULL;
		int										entry_index = -1;
		struct nvmeibt_disk_gpt_partition_entry	temp_entry;

		if (entry_elem->type != JSON_E_DICT) {
			N_Ef(parse_gpt_entry_not_dict, "Entry @INT in @STR is not a dict", i, section_name);
			return -1;
		}

		// Get the index field to know where to place this entry
		entry_dict = &entry_elem->dict;
		for (int j = 0; j < entry_dict->len; j++) {
			if (strcmp(entry_dict->elements[j].key, "index") == 0 &&
				entry_dict->elements[j].value->type == JSON_E_NUM) {
				entry_index = (int)entry_dict->elements[j].value->num;
				break;
			}
		}

		if (entry_index < 0 || entry_index >= gpt->max_n_entries) {
			N_Ef(parse_entry_bad_index, "Entry @INT in @STR has invalid index=@INT (max=@INT)",
				 i, section_name, entry_index, gpt->max_n_entries);
			return -1;
		}

		// Parse the entry (returns: 0=normal, 1=delete, -1=error)
		rv = parse_gpt_entry_from_json(&temp_entry, entry_elem);
		if (rv < 0) {
			N_Ef(parse_entry_failed, "Failed to parse entry @INT in @STR", i, section_name);
			return -1;
		} else if (rv == 1) {
			// Entry marked for deletion - leave gpt->entries[entry_index] as zero (unused)
			N_Tf(parse_entry_delete, "Entry @INT marked for deletion (will be removed)", entry_index);
			continue;
		}

		// Copy normal entry to correct position in entries array
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
	struct mm_json_elem *metadata_gpt_alternate_elem;

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
		N_Ef(validate_json_has_main_alternate, "JSON contains main_gpt_alternate (ambiguous) file=@STR", json_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON must contain only primary copy. Re-export with --gpt-copy=primary" COL_RESET "\n");
		return -1;
	}

	/* Reject metadata_gpt_alternate */
	metadata_gpt_alternate_elem = json_get_dict_value(json_root, "metadata_gpt_alternate");
	if (metadata_gpt_alternate_elem) {
		N_Ef(validate_json_has_metadata_alternate, "JSON contains metadata_gpt_alternate (ambiguous) file=@STR", json_path);
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
		N_Ef(prepare_gpt_parse_failed, "Failed to parse @STR from JSON.", section_name);
		fprintf(stderr, COL_RED_BOLD "ERROR: Failed to parse %s from JSON. Exiting." COL_RESET "\n", section_name);
		return -1;
	}

	/*
	 * Initialize static/constant fields (not in JSON, always the same)
	 * These are the same values set by nvmeibt_disk_metadata_init_gpt_structure()
	 */
	gpt->header.gpt_signature = GPT_SIGNATURE;
	gpt->header.revision = UEFI_GPT_REVISION;
	gpt->header.header_size = UEFI_GPT_HEADER_SIZE;
	gpt->header.size_of_partition_entry = UEFI_MIN_GPT_ENTRY_SIZE;
	gpt->header.n_partition_entries = GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES;		// Open BUG NVMESH-7436 - incorrect value (not the actual number of entries), which causes standard GPT tools to think CRC is corrupted.

	/* Calculate CRCs (exactly as store_gpt will do) */
	gpt->header.partition_entry_array_crc32 = crc32_seedless(gpt->entries,
															 gpt->header.n_partition_entries * gpt->header.size_of_partition_entry);
	gpt->header.header_crc32 = 0;
	gpt->header.header_crc32 = crc32_seedless(&gpt->header, gpt->header.header_size);

	return 0;
}

// Helper: Parse UUID from JSON or preserve current value
static void json_parse_uuid_or_preserve(union nvmeib_uuid *dst,
										struct mm_json_elem *json_elem,
										const char *key,
										const union nvmeib_uuid *current_val)
{
	const char *uuid_str = json_get_dict_str(json_elem, key, NULL);

	if (uuid_str) {
		nvmeibt_urn_uuid_str_to_union_uuid(dst, uuid_str);
	} else {
		*dst = *current_val;		// Preserve if missing
	}
}

// Helper: Parse string from JSON or preserve current value
static void json_parse_str_or_preserve(char *dst,
									   size_t dst_size,
									   struct mm_json_elem *json_elem,
									   const char *key,
									   const char *current_val)
{
	const char *str = json_get_dict_str(json_elem, key, NULL);

	if (str) {
		nvmeibt_strlcpy(dst, str, dst_size);
	} else {
		nvmeibt_strlcpy(dst, current_val, dst_size);		// Preserve if missing
	}
}

/**
 * Prepare disk_metadata from JSON section
 * Parses JSON, preserves readonly fields from current, returns prepared structure
 * Returns 0 on success, -1 on error
 */
static int prepare_disk_metadata_from_json(struct nvmeibt_disk_metadata *prepared_dm,
											const struct nvmeibt_disk_metadata *current_dm,
											struct mm_json_elem *disk_metadata_elem)
{
	memset(prepared_dm, 0, sizeof(*prepared_dm));

	/* Initialize static fields */
	prepared_dm->signature = DISK_METADATA_SIGNATURE;

	/* Parse editable fields from JSON (default to current if missing) */
	json_parse_uuid_or_preserve(&prepared_dm->mgmt_db_uuid, disk_metadata_elem, "mgmt_db_uuid", &current_dm->mgmt_db_uuid);
	json_parse_uuid_or_preserve(&prepared_dm->native_nguid_unused, disk_metadata_elem, "native_nguid", &current_dm->native_nguid_unused);
	json_parse_str_or_preserve(prepared_dm->ldisk_id_str, sizeof(prepared_dm->ldisk_id_str), disk_metadata_elem, "ldisk_id_str", current_dm->ldisk_id_str);
	json_parse_str_or_preserve(prepared_dm->native_serial_str, sizeof(prepared_dm->native_serial_str), disk_metadata_elem, "native_serial_str", current_dm->native_serial_str);

	prepared_dm->disk_metadata_version = (unsigned int)json_get_dict_num(disk_metadata_elem, "disk_metadata_version", current_dm->disk_metadata_version);
	prepared_dm->format_pblk_size = (unsigned int)json_get_dict_num(disk_metadata_elem, "format_pblk_size", current_dm->format_pblk_size);
	prepared_dm->format_metadata_size = (unsigned int)json_get_dict_num(disk_metadata_elem, "format_metadata_size", current_dm->format_metadata_size);
	prepared_dm->is_md_supported = json_get_dict_bool(disk_metadata_elem, "is_md_supported", current_dm->is_md_supported);

	/* Parse WARNING fields (default to current if missing) */
	prepared_dm->last_pba_zeroed = (uint64_t)json_get_dict_num(disk_metadata_elem, "_WARNING_last_pba_zeroed", current_dm->last_pba_zeroed);
	prepared_dm->format_request_counter = (unsigned int)json_get_dict_num(disk_metadata_elem, "_WARNING_format_request_counter", current_dm->format_request_counter);

	/* Preserve readonly fields from current disk (hardware-derived) */
	prepared_dm->nsid = current_dm->nsid;

	/* Calculate CRC */
	prepared_dm->crc32 = 0;
	prepared_dm->crc32 = crc32_seedless(prepared_dm, sizeof(*prepared_dm));

	return 0;
}

/**
 * Prepare segment metadata control block from JSON
 * Returns 0 on success, -1 on error
 */
static int prepare_segment_metadata_from_json(struct nvmeibt_seg_active_metadata_ctrl *prepared_seg_md,
											   const struct nvmeibt_seg_active_metadata_ctrl *current_seg_md,
											   struct mm_json_elem *seg_md_json_elem)
{
	memset(prepared_seg_md, 0, sizeof(*prepared_seg_md));

	/* Preserve static fields from current (constants that never change) */
	nvmeibt_strlcpy(prepared_seg_md->header.magic_str, current_seg_md->header.magic_str, sizeof(prepared_seg_md->header.magic_str));

	/* Parse editable fields from JSON (default to current if missing) */
	json_parse_uuid_or_preserve(&prepared_seg_md->header.mgmt_db_uuid, seg_md_json_elem, "mgmt_db_uuid", &current_seg_md->header.mgmt_db_uuid);
	json_parse_str_or_preserve(prepared_seg_md->hostname, sizeof(prepared_seg_md->hostname), seg_md_json_elem, "hostname", current_seg_md->hostname);

	/* Parse WARNING fields (default to current if missing) */
	json_parse_uuid_or_preserve(&prepared_seg_md->disk_segment_uuid, seg_md_json_elem, "_WARNING_disk_segment_uuid", &current_seg_md->disk_segment_uuid);
	prepared_seg_md->reservation_mode_version = (uint64_t)json_get_dict_num(seg_md_json_elem, "_WARNING_reservation_mode_version", current_seg_md->reservation_mode_version);
	prepared_seg_md->active_praid_version_major = (int)json_get_dict_num(seg_md_json_elem, "_WARNING_active_praid_version_major", current_seg_md->active_praid_version_major);
	prepared_seg_md->active_praid_version_minor = (int)json_get_dict_num(seg_md_json_elem, "_WARNING_active_praid_version_minor", current_seg_md->active_praid_version_minor);
	prepared_seg_md->is_current_shutdown_clean = json_get_dict_bool(seg_md_json_elem, "_WARNING_is_current_shutdown_clean", current_seg_md->is_current_shutdown_clean);
	prepared_seg_md->n_blksets_scrubbed = (uint64_t)json_get_dict_num(seg_md_json_elem, "_WARNING_n_blksets_scrubbed", current_seg_md->n_blksets_scrubbed);

	/* Preserve readonly fields from current (version-dependent, calculated, or internal state) */
	prepared_seg_md->header.software_version = current_seg_md->header.software_version;
	prepared_seg_md->header.seg_metadata_version = current_seg_md->header.seg_metadata_version;
	prepared_seg_md->save_timespec_tv_sec = current_seg_md->save_timespec_tv_sec;
	prepared_seg_md->locks_table_pbyte_s = current_seg_md->locks_table_pbyte_s;
	prepared_seg_md->metadata_pbyte_s = current_seg_md->metadata_pbyte_s;
	prepared_seg_md->is_written_on_disk = current_seg_md->is_written_on_disk;
	prepared_seg_md->committed_praid_config_version = current_seg_md->committed_praid_config_version;
	prepared_seg_md->reserved_was_is_zeroed_after_delete = current_seg_md->reserved_was_is_zeroed_after_delete;
	prepared_seg_md->UNUSED__Before_3_2_this_and_the_prev_field_were_struct_timecal__16_bytes_together_on_all_machines =
		current_seg_md->UNUSED__Before_3_2_this_and_the_prev_field_were_struct_timecal__16_bytes_together_on_all_machines;

	/* Calculate CRC (matches production code in nvmeibt_ds_metadata.c) */
	prepared_seg_md->metadata_ctrl_crc32 = crc32_seedless(prepared_seg_md, offsetof(typeof(*prepared_seg_md), metadata_ctrl_crc32));
	prepared_seg_md->locks_table_crc32 = current_seg_md->locks_table_crc32;  // Preserve locks table CRC

	return 0;
}

/**
 * Compare segment metadata control blocks and display diff
 * Returns number of changes detected (or 0 if no changes)
 */
static int compare_and_show_segment_metadata_diff(const struct nvmeibt_seg_active_metadata_ctrl *current,
												   const struct nvmeibt_seg_active_metadata_ctrl *json_data,
												   const char *partition_name)
{
	int									n_changes = 0;
	struct nvmeibt_urn_uuid				current_uuid_urn;
	struct nvmeibt_urn_uuid				json_uuid_urn;

	fprintf(stdout, "\n=== Segment Metadata: %s ===\n", partition_name);

	/* Check editable fields */
	if (memcmp(&current->header.mgmt_db_uuid, &json_data->header.mgmt_db_uuid, sizeof(current->header.mgmt_db_uuid)) != 0) {
		current_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&current->header.mgmt_db_uuid);
		json_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&json_data->header.mgmt_db_uuid);
		fprintf(stdout, "  - mgmt_db_uuid: %s -> %s\n", current_uuid_urn.str, json_uuid_urn.str);
		n_changes++;
	}
	if (strcmp(current->hostname, json_data->hostname) != 0) {
		fprintf(stdout, "  - hostname: %s -> %s\n", current->hostname, json_data->hostname);
		n_changes++;
	}

	/* Check WARNING fields */
	if (memcmp(&current->disk_segment_uuid, &json_data->disk_segment_uuid, sizeof(current->disk_segment_uuid)) != 0) {
		current_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&current->disk_segment_uuid);
		json_uuid_urn = nvmeibt_union_uuid_to_urn_uuid(&json_data->disk_segment_uuid);
		fprintf(stdout, COL_YELLOW "  [WARNING] disk_segment_uuid: %s -> %s" COL_RESET "\n", current_uuid_urn.str, json_uuid_urn.str);
		n_changes++;
	}
	if (current->reservation_mode_version != json_data->reservation_mode_version) {
		fprintf(stdout, COL_YELLOW "  [WARNING] reservation_mode_version: %llu -> %llu" COL_RESET "\n",
				current->reservation_mode_version, json_data->reservation_mode_version);
		n_changes++;
	}
	if (current->active_praid_version_major != json_data->active_praid_version_major) {
		fprintf(stdout, COL_YELLOW "  [WARNING] active_praid_version_major: %d -> %d" COL_RESET "\n",
				current->active_praid_version_major, json_data->active_praid_version_major);
		n_changes++;
	}
	if (current->active_praid_version_minor != json_data->active_praid_version_minor) {
		fprintf(stdout, COL_YELLOW "  [WARNING] active_praid_version_minor: %d -> %d" COL_RESET "\n",
				current->active_praid_version_minor, json_data->active_praid_version_minor);
		n_changes++;
	}
	if (current->is_current_shutdown_clean != json_data->is_current_shutdown_clean) {
		fprintf(stdout, COL_YELLOW "  [WARNING] is_current_shutdown_clean: %s -> %s" COL_RESET "\n",
				current->is_current_shutdown_clean ? "true" : "false",
				json_data->is_current_shutdown_clean ? "true" : "false");
		n_changes++;
	}
	if (current->n_blksets_scrubbed != json_data->n_blksets_scrubbed) {
		fprintf(stdout, COL_YELLOW "  [WARNING] n_blksets_scrubbed: %llu -> %llu" COL_RESET "\n",
				(unsigned long long)current->n_blksets_scrubbed, (unsigned long long)json_data->n_blksets_scrubbed);
		n_changes++;
	}

	if (n_changes == 0) {
		fprintf(stdout, "  No changes\n");
	}

	return n_changes;
}

/**
 * Compare disk_metadata structures and display diff
 * Returns number of changes detected (or 0 if no changes)
 */
static int compare_and_show_disk_metadata_diff(const struct nvmeibt_disk_metadata *current,
												const struct nvmeibt_disk_metadata *json_data)
{
	int n_changes = 0;

	fprintf(stdout, "\n=== disk_metadata Changes ===\n");

	/* Check all editable fields */
	if (memcmp(&current->mgmt_db_uuid, &json_data->mgmt_db_uuid, sizeof(current->mgmt_db_uuid)) != 0) {
		struct nvmeibt_urn_uuid current_uuid = nvmeibt_union_uuid_to_urn_uuid(&current->mgmt_db_uuid);
		struct nvmeibt_urn_uuid json_uuid = nvmeibt_union_uuid_to_urn_uuid(&json_data->mgmt_db_uuid);
		fprintf(stdout, "  - mgmt_db_uuid: %s -> %s\n", current_uuid.str, json_uuid.str);
		n_changes++;
	}
	if (strcmp(current->ldisk_id_str, json_data->ldisk_id_str) != 0) {
		fprintf(stdout, "  - ldisk_id_str: %s -> %s\n", current->ldisk_id_str, json_data->ldisk_id_str);
		n_changes++;
	}
	if (strcmp(current->native_serial_str, json_data->native_serial_str) != 0) {
		fprintf(stdout, "  - native_serial_str: %s -> %s\n", current->native_serial_str, json_data->native_serial_str);
		n_changes++;
	}
	if (current->disk_metadata_version != json_data->disk_metadata_version) {
		fprintf(stdout, "  - disk_metadata_version: %u -> %u\n", current->disk_metadata_version, json_data->disk_metadata_version);
		n_changes++;
	}
	if (current->format_pblk_size != json_data->format_pblk_size) {
		fprintf(stdout, "  - format_pblk_size: %u -> %u\n", current->format_pblk_size, json_data->format_pblk_size);
		n_changes++;
	}
	if (current->format_metadata_size != json_data->format_metadata_size) {
		fprintf(stdout, "  - format_metadata_size: %u -> %u\n", current->format_metadata_size, json_data->format_metadata_size);
		n_changes++;
	}
	if (current->is_md_supported != json_data->is_md_supported) {
		fprintf(stdout, "  - is_md_supported: %s -> %s\n", current->is_md_supported ? "true" : "false", json_data->is_md_supported ? "true" : "false");
		n_changes++;
	}

	/* Check WARNING fields */
	if (current->last_pba_zeroed != json_data->last_pba_zeroed) {
		fprintf(stdout, COL_YELLOW "  [WARNING] last_pba_zeroed: %lu -> %lu" COL_RESET "\n", current->last_pba_zeroed, json_data->last_pba_zeroed);
		n_changes++;
	}
	if (current->format_request_counter != json_data->format_request_counter) {
		fprintf(stdout, COL_YELLOW "  [WARNING] format_request_counter: %u -> %u" COL_RESET "\n", current->format_request_counter, json_data->format_request_counter);
		n_changes++;
	}

	if (n_changes == 0) {
		fprintf(stdout, COL_GREEN "  No changes detected" COL_RESET "\n");
	}

	return n_changes;
}

/**
 * Compare GPT structures and display diff
 * Returns number of changes detected
 */
static int compare_and_show_gpt_diff(const struct nvmeibt_disk_gpt *disk_gpt,
									 const struct nvmeibt_disk_gpt *json_gpt,
									 const char *gpt_name)
{
	int		n_additions = 0;
	int		n_deletions = 0;
	int		n_modifications = 0;
	int		n_unchanged = 0;

	fprintf(stdout, "\n=== %s Changes ===\n", gpt_name);
	fprintf(stdout, "CRC: header 0x%08x->0x%08x, entries 0x%08x->0x%08x\n",
			disk_gpt->header.header_crc32, json_gpt->header.header_crc32,
			disk_gpt->header.partition_entry_array_crc32, json_gpt->header.partition_entry_array_crc32);

	for (int i = 0; i < json_gpt->max_n_entries; i++) {
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
	fprintf(stdout, "  - Additions:     %d\n", n_additions);
	fprintf(stdout, "  - Deletions:     %d\n", n_deletions);
	fprintf(stdout, "  - Modifications: %d\n", n_modifications);
	fprintf(stdout, "  - Unchanged:     %d\n", n_unchanged);

	if (n_additions + n_deletions + n_modifications == 0) {
		fprintf(stdout, COL_GREEN "No changes detected" COL_RESET "\n");
	}

	return (n_additions + n_deletions + n_modifications);
}

// Helper structure to store prepared segment metadata for apply
struct prepared_seg_md_entry {
	struct nvmeibt_seg_active_metadata_ctrl		prepared_data;
	const struct nvmeibt_disk_gpt_partition_entry	*partition;
	const char									*partition_name;
	int											has_changes;
};

/**
 * Process segment metadata from JSON and prepare for apply
 * Returns: number of changes detected (>= 0), or -1 on fatal error
 */
static int process_segment_metadata_from_json(
	struct mm_json_elem *seg_metadata_partitions_elem,
	struct nvmeibt_disk_gpt *current_metadata_gpt,
	struct gpt_util_config *config,
	int disk_fd,
	struct prepared_seg_md_entry **out_prepared,
	int *out_n_prepared)
{
	int		n_seg_md_partitions;
	int		n_segment_metadata_changes = 0;
	int		n_prepared_seg_mds = 0;
	struct prepared_seg_md_entry	*prepared_seg_mds = NULL;

	if (!seg_metadata_partitions_elem || seg_metadata_partitions_elem->type != JSON_E_ARRAY) {
		*out_prepared = NULL;
		*out_n_prepared = 0;
		return 0;		// No segment metadata in JSON
	}

	n_seg_md_partitions = seg_metadata_partitions_elem->array.len;

	if (n_seg_md_partitions > 0) {
		/* Sanity check: metadata GPT has MAX_NUM_GPT_ENTRIES limit, minus 1 for disk_metadata */
		if (n_seg_md_partitions > MAX_NUM_GPT_ENTRIES - 1) {
			N_Ef(apply_seg_md_count_overflow, "Too many segment metadata partitions in JSON: @INT (max=@INT)",
				 n_seg_md_partitions, MAX_NUM_GPT_ENTRIES - 1);
			fprintf(stderr, COL_RED_BOLD "ERROR: JSON contains too many segment metadata partitions (%d, max=%d)" COL_RESET "\n",
					n_seg_md_partitions, MAX_NUM_GPT_ENTRIES - 1);
			return -1;
		}

		fprintf(stdout, "\nProcessing %d segment metadata partition%s...\n",
				n_seg_md_partitions, n_seg_md_partitions == 1 ? "" : "s");

		// Allocate array to store prepared segment metadata for write phase
		prepared_seg_mds = NNVMEIBT_BM_CALLOC(trace_apply_seg_md_array, n_seg_md_partitions * sizeof(*prepared_seg_mds));
		if (!prepared_seg_mds) {
			N_Ef(apply_seg_md_alloc_failed, "Failed to allocate prepared segment metadata array size=@SIZE_T",
				 n_seg_md_partitions * sizeof(*prepared_seg_mds));
			fprintf(stderr, COL_RED_BOLD "ERROR: Memory allocation failed for segment metadata processing" COL_RESET "\n");
			return -1;
		}
	}

	for (int seg_idx = 0; seg_idx < n_seg_md_partitions; seg_idx++) {
		struct mm_json_elem								*seg_json = seg_metadata_partitions_elem->array.elements[seg_idx];
		const char										*json_partition_name;
		const char										*json_seg_uuid_str;
		union nvmeib_uuid								json_seg_uuid;
		struct nvmeibt_seg_active_metadata_ctrl			current_seg_md;
		struct nvmeibt_seg_active_metadata_ctrl			prepared_seg_md;
		const struct nvmeibt_disk_gpt_partition_entry	*seg_md_partition = NULL;
		uint64_t										seg_md_pbyte_s;
		int												seg_changes;
		uint64_t										json_pba_s;
		uint64_t										json_pba_e;

		if (!seg_json || seg_json->type != JSON_E_DICT) {
			continue;
		}

		// Get partition identifiers from JSON
		json_partition_name = json_get_dict_str(seg_json, "_READONLY_partition_name", NULL);
		json_seg_uuid_str = json_get_dict_str(seg_json, "_WARNING_disk_segment_uuid", NULL);
		if (!json_partition_name || !json_seg_uuid_str) {
			N_Wf(apply_seg_md_missing_id, "Segment metadata entry missing partition_name or disk_segment_uuid, skipping");
			fprintf(stderr, COL_YELLOW "Warning: Skipping segment metadata entry %d (missing identifiers)" COL_RESET "\n", seg_idx);
			continue;
		}

		// Get PBA range from JSON for validation
		json_pba_s = (uint64_t)json_get_dict_num(seg_json, "_READONLY_partition_pba_s", 0);
		json_pba_e = (uint64_t)json_get_dict_num(seg_json, "_READONLY_partition_pba_e", 0);

		// Parse segment UUID from JSON
		nvmeibt_urn_uuid_str_to_union_uuid(&json_seg_uuid, json_seg_uuid_str);

		// Find matching partition on disk by UUID, validate PBA range
		for (int k = 0; k < current_metadata_gpt->max_n_entries; k++) {
			const struct nvmeibt_disk_gpt_partition_entry	*entry = &current_metadata_gpt->entries[k];
			struct nvmeibt_seg_active_metadata_ctrl			temp_seg_md;
			uint64_t										temp_pbyte_s;

			if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(entry)) {
				continue;
			}
			if (!ARE_UUID_EQ(&entry->partition_type_guid, &EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID)) {
				continue;
			}

			/* Read segment metadata control block to get its UUID */
			memset(&temp_seg_md, 0, sizeof(temp_seg_md));
			temp_pbyte_s = entry->pba_s * config->pblk_size;
			if (nvmeibt_ds_metadata_ctrl_blk_read(NULL, disk_fd, config->pblk_size, temp_pbyte_s, &temp_seg_md) < 0) {
				N_Wf(apply_seg_md_read_for_match_failed, "Failed to read segment metadata for UUID matching at pba=@LLU", entry->pba_s);
				continue;		/* Skip this entry if we can't read it */
			}

			// Match by segment UUID (primary key)
			if (ARE_UUID_EQ(&temp_seg_md.disk_segment_uuid, &json_seg_uuid)) {
				/* Validate PBA range matches (safety check) */
				if (json_pba_s != entry->pba_s || json_pba_e != entry->pba_e) {
					N_Wf(apply_seg_md_pba_mismatch, "Segment metadata PBA mismatch: uuid=@UUID_LE json_pba=@PBA_S-@PBA_E disk_pba=@PBA_S-@PBA_E",
						 &json_seg_uuid, json_pba_s, json_pba_e, entry->pba_s, entry->pba_e);
					fprintf(stderr, COL_YELLOW "Warning: Segment '%s' has PBA mismatch (JSON: %lu-%lu, Disk: %lu-%lu), skipping for safety" COL_RESET "\n",
							json_partition_name, json_pba_s, json_pba_e, entry->pba_s, entry->pba_e);
					break;		/* Don't match this entry */
				}
				seg_md_partition = entry;
				break;
			}
		}

		if (!seg_md_partition) {
			N_Wf(apply_seg_md_not_found, "Segment metadata partition not found on disk: name=@STR", json_partition_name);
			fprintf(stderr, COL_YELLOW "Warning: Segment partition '%s' not found on disk, skipping" COL_RESET "\n", json_partition_name);
			continue;
		}

		/* Read current segment metadata from disk */
		memset(&current_seg_md, 0, sizeof(current_seg_md));
		seg_md_pbyte_s = seg_md_partition->pba_s * config->pblk_size;
		if (nvmeibt_ds_metadata_ctrl_blk_read(NULL, disk_fd, config->pblk_size, seg_md_pbyte_s, &current_seg_md) < 0) {
			N_Wf(apply_seg_md_read_failed, "Failed to read segment metadata: name=@STR pba=@LLU", json_partition_name, seg_md_partition->pba_s);
			fprintf(stderr, COL_YELLOW "Warning: Cannot read segment metadata '%s', skipping" COL_RESET "\n", json_partition_name);
			continue;
		}

		/* Prepare from JSON */
		memset(&prepared_seg_md, 0, sizeof(prepared_seg_md));
		if (prepare_segment_metadata_from_json(&prepared_seg_md, &current_seg_md, seg_json) < 0) {
			fprintf(stderr, COL_YELLOW "Warning: Failed to prepare segment metadata '%s', skipping" COL_RESET "\n", json_partition_name);
			continue;
		}

		/* Compare and show diff */
		seg_changes = compare_and_show_segment_metadata_diff(&current_seg_md, &prepared_seg_md, json_partition_name);
		n_segment_metadata_changes += seg_changes;

		/* Store prepared data for write phase (if there are changes) */
		if (seg_changes > 0 && prepared_seg_mds) {
			prepared_seg_mds[n_prepared_seg_mds].prepared_data = prepared_seg_md;
			prepared_seg_mds[n_prepared_seg_mds].partition = seg_md_partition;
			prepared_seg_mds[n_prepared_seg_mds].partition_name = json_partition_name;
			prepared_seg_mds[n_prepared_seg_mds].has_changes = 1;
			n_prepared_seg_mds++;
		}
	}

	*out_prepared = prepared_seg_mds;
	*out_n_prepared = n_prepared_seg_mds;
	return n_segment_metadata_changes;
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
	struct mm_json_elem			*json_root = NULL;
	struct mm_json_elem			*main_gpt_primary_elem = NULL;
	struct mm_json_elem			*metadata_gpt_primary_elem = NULL;
	struct mm_json_elem			*disk_metadata_elem = NULL;
	struct mm_json_elem			*seg_metadata_partitions_elem = NULL;
	const struct nvmeibt_disk_gpt_partition_entry *metadata_partition = NULL;
	const struct nvmeibt_disk_gpt_partition_entry *disk_md_partition = NULL;
	struct nvmeibt_disk_gpt		current_main_gpt;
	struct nvmeibt_disk_gpt		current_metadata_gpt;
	struct nvmeibt_disk_gpt		json_main_gpt;
	struct nvmeibt_disk_gpt		json_metadata_gpt;
	struct nvmeibt_disk_metadata current_disk_md __attribute__((aligned(PAGE_SIZE)));
	struct nvmeibt_disk_metadata prepared_disk_md __attribute__((aligned(PAGE_SIZE)));
	struct prepared_seg_md_entry *prepared_seg_mds = NULL;
	int							n_prepared_seg_mds = 0;
	int							n_main_changes = 0;
	int							n_metadata_changes = 0;
	int							n_disk_metadata_changes = 0;
	int							n_segment_metadata_changes = 0;
	int							total_changes = 0;
	uint64_t					pbyte_s = 0;
	const char					*json_serial_num = NULL;
	const char					*json_version = NULL;

	fprintf(stdout, "\n=== Applying GPT from JSON: %s ===\n", config->apply_json_file);
	fprintf(stdout, "Device: %s\n", config->device_path);
	if (config->write_mode) {
		fprintf(stdout, "Mode: " COL_YELLOW "WRITE" COL_RESET " (changes will be applied to disk)\n");
	} else {
		fprintf(stdout, "Mode: " COL_YELLOW "DRY-RUN" COL_RESET " (use --write to apply changes)\n");
		N_Tf(apply_dry_run, "Dry-run apply: dev=@STR json=@STR", config->device_path, config->apply_json_file);
	}
	fprintf(stdout, "\n");

	/* Step 1: Load and parse JSON file */
	json_root = load_json_file_or_fail(config->apply_json_file);
	if (!json_root) {
		rv = -1;
		goto out;
	}

	/* Step 2: Validate gpt_util version compatibility */
	json_version = json_get_dict_str(json_root, "gpt_util_version", NULL);
	if (!json_version) {
		N_Ef(apply_json_no_version, "JSON missing gpt_util_version field file=@STR", config->apply_json_file);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON missing gpt_util_version field. Exiting." COL_RESET "\n");
		rv = -1;
		goto out;
	} else {
		fprintf(stdout, "JSON version: %s (current: %s)\n", json_version, GPT_UTIL_VERSION);
		if (strcmp(json_version, GPT_UTIL_VERSION) != 0) {
			N_Ef(apply_json_version_mismatch, "Version mismatch: json=@STR current=@STR file=@STR",
					json_version, GPT_UTIL_VERSION, config->apply_json_file);
			fprintf(stderr, COL_RED_BOLD "ERROR: JSON was created by different gpt_util version. Exiting." COL_RESET "\n");
			rv = -1;
			goto out;
		}
	}

	/* Step 3: Validate device has required NVMesh structure */
	if (validate_nvmesh_device_structure(disk_fd, config->pblk_size, config->pba_s, config->pba_hw_e,
										  &current_main_gpt, &metadata_partition) < 0) {
		rv = -1;
		goto out;
	}

	/* Step 4: Validate JSON has required sections (fail fast) */
	if (validate_json_required_sections(json_root, config->apply_json_file,
										 &main_gpt_primary_elem, &metadata_gpt_primary_elem) < 0) {
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
		fprintf(stderr, COL_RED_BOLD "ERROR: Failed to read current Metadata GPT. Exiting." COL_RESET "\n");
		rv = -1;
		goto out;
	}

	if (prepare_gpt_from_json(&json_metadata_gpt, metadata_gpt_primary_elem, METADATA_GPT_NAME, MAX_NUM_GPT_ENTRIES) < 0) {
		rv = -1;
		goto out;
	}

	/* Step 7: Validate serial number (REQUIRED - fail-closed for safety) */
	json_serial_num = json_get_dict_str(json_root, "_READONLY_controller_serial_num", NULL);
	if (validate_serial_number_match(disk_fd, config, json_serial_num, "JSON") < 0) {
		rv = -1;
		goto out;
	}

	/* Step 8: Compare and show differences */
	n_main_changes = compare_and_show_gpt_diff(&current_main_gpt, &json_main_gpt, "Main GPT");
	fprintf(stdout, "\n");
	n_metadata_changes = compare_and_show_gpt_diff(&current_metadata_gpt, &json_metadata_gpt, "Metadata GPT");
	disk_metadata_elem = json_get_dict_value(json_root, "disk_metadata");
	if (!disk_metadata_elem) {
		N_Ef(apply_no_disk_metadata, "JSON missing disk_metadata section (cannot validate device) file=@STR", config->apply_json_file);
		fprintf(stderr, COL_RED_BOLD "ERROR: JSON must contain disk_metadata section with _READONLY_native_serial_str" COL_RESET "\n");
		rv = -1;
		goto out;
	}
	disk_md_partition = nvmeibt_disk_metadata_get_disk_metadata_entry(&current_metadata_gpt);
	if (!disk_md_partition) {
		N_Ef(apply_no_disk_md_partition, "Device missing disk_metadata partition (cannot validate serial) dev=@STR", config->device_path);
		fprintf(stderr, COL_RED_BOLD "ERROR: Device has no disk_metadata partition" COL_RESET "\n");
		rv = -1;
		goto out;
	}
		pbyte_s = disk_md_partition->pba_s * config->pblk_size;

		memset(&current_disk_md, 0, sizeof(current_disk_md));
		memset(&prepared_disk_md, 0, sizeof(prepared_disk_md));

		if (nvmeibt_disk_metadata_read_disk_metadata(NULL, disk_fd, config->pblk_size, pbyte_s, &current_disk_md) == 0 &&
			prepare_disk_metadata_from_json(&prepared_disk_md, &current_disk_md, disk_metadata_elem) == 0) {
			n_disk_metadata_changes = compare_and_show_disk_metadata_diff(&current_disk_md, &prepared_disk_md);
	}
	/* Step 9: Process segment metadata partitions from JSON */
	seg_metadata_partitions_elem = json_get_dict_value(json_root, "segment_metadata_partitions");
	n_segment_metadata_changes = process_segment_metadata_from_json(
		seg_metadata_partitions_elem, &current_metadata_gpt, config, disk_fd,
		&prepared_seg_mds, &n_prepared_seg_mds);
	if (n_segment_metadata_changes < 0) {
		rv = -1;
		goto out;
	}

	/* Step 10: Write if in write mode */
	total_changes = n_main_changes + n_metadata_changes + n_disk_metadata_changes + n_segment_metadata_changes;
	if (config->write_mode) {
		if (total_changes == 0) {
			fprintf(stdout, "\n" COL_GREEN "=== No Changes Detected - Skipping Write ===" COL_RESET "\n");
			fprintf(stdout, "JSON matches current disk state.\n");
			rv = 0;
			goto out;
		} else {
			char operation_desc[256];
			char backup_path[512];
			snprintf(operation_desc, sizeof(operation_desc), "Apply %d change%s from JSON",
					 total_changes, total_changes == 1 ? "" : "s");
			/* Confirm before writing */
			if (!validate_and_confirm_write(config, operation_desc)) {
				rv = -1;
				goto out;
			}

			/* Create binary backup before modifying */
			if (create_binary_backup(disk_fd, config, backup_path, sizeof(backup_path)) < 0) {
				fprintf(stderr, COL_RED_BOLD "ERROR: Backup failed - aborting write operation for safety" COL_RESET "\n");
				rv = -1;
				goto out;
			}

			fprintf(stdout, "\n" COL_GREEN "=== Writing Changes to Disk ===" COL_RESET "\n");
		}

		/* Write Main GPT only if there are changes */
		if (n_main_changes > 0) {
			/* Copy JSON data into current_main_gpt (which has correct location fields) */
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
		}

		/* Write Metadata GPT only if there are changes */
		if (n_metadata_changes > 0) {
			/* Copy JSON data into current_metadata_gpt (which has correct location fields) */
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
		}

		/* Write disk_metadata if there are changes */
		if (n_disk_metadata_changes > 0) {
			char		*dma_buffer = NULL;
			int			n_bytes_write;

			pbyte_s = disk_md_partition->pba_s * config->pblk_size;
			/* Write disk_metadata - Audit trail log */
			N_IMf(apply_disk_md_write, "Applying disk_metadata from JSON: dev=@STR json=@STR changes=@INT CRC_new=@CRC",
				  config->device_path, config->apply_json_file, n_disk_metadata_changes, prepared_disk_md.crc32);

			n_bytes_write = roundup(sizeof(prepared_disk_md), config->pblk_size);
			dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_apply_disk_md, PAGE_SIZE, n_bytes_write);
			memcpy(dma_buffer, &prepared_disk_md, sizeof(prepared_disk_md));

			if (NNVMEIBT_PWRITE(trace_apply_disk_md_write, disk_fd, dma_buffer, n_bytes_write, pbyte_s, 0) != (ssize_t)n_bytes_write) {
				N_Ef(apply_disk_md_write_failed, "Failed to write disk_metadata dev=@STR", config->device_path);
				fprintf(stderr, COL_RED_BOLD "ERROR: Failed to write disk_metadata to disk" COL_RESET "\n");
				NNVMEIBT_BM_FREE(trace_apply_disk_md_free, dma_buffer);
				rv = -1;
				goto out;
			}

			NNVMEIBT_BM_FREE(trace_apply_disk_md_free2, dma_buffer);
		}

		/* Write segment metadata partitions if there are changes (use prepared data from diff phase) */
		if (n_segment_metadata_changes > 0 && prepared_seg_mds) {
			for (int i = 0; i < n_prepared_seg_mds; i++) {
				char		*dma_buffer = NULL;
				int			n_bytes_write;
				uint64_t	seg_md_pbyte_s;

				if (!prepared_seg_mds[i].has_changes) {
					continue;
				}

				seg_md_pbyte_s = prepared_seg_mds[i].partition->pba_s * config->pblk_size;

				/* Write segment metadata - Audit trail log */
				N_IMf(apply_seg_md_write, "Applying segment metadata from JSON: dev=@STR partition=@STR CRC_new=@CRC",
					  config->device_path, prepared_seg_mds[i].partition_name, prepared_seg_mds[i].prepared_data.metadata_ctrl_crc32);

				n_bytes_write = roundup(sizeof(prepared_seg_mds[i].prepared_data), config->pblk_size);
				dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_apply_seg_md, PAGE_SIZE, n_bytes_write);
				memcpy(dma_buffer, &prepared_seg_mds[i].prepared_data, sizeof(prepared_seg_mds[i].prepared_data));

				if (NNVMEIBT_PWRITE(trace_apply_seg_md_write, disk_fd, dma_buffer, n_bytes_write, seg_md_pbyte_s, 0) != (ssize_t)n_bytes_write) {
					N_Ef(apply_seg_md_write_failed, "Failed to write segment metadata: partition=@STR dev=@STR",
						 prepared_seg_mds[i].partition_name, config->device_path);
					fprintf(stderr, COL_RED_BOLD "ERROR: Failed to write segment metadata '%s' to disk" COL_RESET "\n",
							prepared_seg_mds[i].partition_name);
					NNVMEIBT_BM_FREE(trace_apply_seg_md_free, dma_buffer);
					rv = -1;
					goto out;
				}

				NNVMEIBT_BM_FREE(trace_apply_seg_md_free2, dma_buffer);
			}
		}

		if (total_changes > 0) {
			fprintf(stdout, "\n" COL_GREEN "=== Changes Successfully Applied ===" COL_RESET "\n");
			fprintf(stdout, "Device: %s\n", config->device_path);
			fprintf(stdout, "  Main GPT:               %d change%s\n", n_main_changes, n_main_changes == 1 ? "" : "s");
			fprintf(stdout, "  Metadata GPT:           %d change%s\n", n_metadata_changes, n_metadata_changes == 1 ? "" : "s");
			fprintf(stdout, "  disk_metadata:          %d change%s\n", n_disk_metadata_changes, n_disk_metadata_changes == 1 ? "" : "s");
			fprintf(stdout, "  segment_metadata (all): %d change%s\n", n_segment_metadata_changes, n_segment_metadata_changes == 1 ? "" : "s");
		}
	} else {
		fprintf(stdout, "\n" COL_GREEN "=== Dry-Run Complete ===" COL_RESET "\n");
		if (total_changes > 0) {
			fprintf(stdout, COL_YELLOW "Use --write to apply %d change%s to disk." COL_RESET "\n",
					total_changes, total_changes == 1 ? "" : "s");
		} else {
			fprintf(stdout, "No changes detected.\n");
		}
	}

	rv = 0;

out:
	if (json_root) {
		nvmeibt_mm_json_free_kv_tree(json_root);
	}
	NNVMEIBT_BM_FREE(trace_apply_seg_md_array_free, prepared_seg_mds);
	return rv;
}

/**
 * Helper: Restore a single structure from file to device
 * Returns 0 on success, -1 on error
 */
static int restore_structure(int disk_fd, const char *filepath, uint64_t pba_start, uint64_t n_blocks, int pblk_size)
{
	int			rv = -1;
	int			backup_fd = -1;
	char		*restore_buffer = NULL;
	uint64_t	restore_size_bytes;
	uint64_t	pbyte_start;
	struct stat	st;

	/* Verify file exists and get size */
	if (stat(filepath, &st) < 0) {
		N_Ef(restore_struct_stat_failed, "Cannot stat @STR @AUTO_ERRNO", filepath);
		fprintf(stderr, COL_RED_BOLD "ERROR: Structure file not found: %s" COL_RESET "\n", filepath);
		goto out;
	}

	restore_size_bytes = n_blocks * pblk_size;
	pbyte_start = pba_start * pblk_size;

	/* Validate file size */
	if ((uint64_t)st.st_size != restore_size_bytes) {
		N_Ef(restore_struct_size_mismatch, "File size mismatch: @STR expected=@SIZE_T actual=@SIZE_T",
			 filepath, restore_size_bytes, (uint64_t)st.st_size);
		fprintf(stderr, COL_RED_BOLD "ERROR: Structure file size mismatch: %s" COL_RESET "\n", filepath);
		goto out;
	}

	restore_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_restore_struct_buf, PAGE_SIZE, restore_size_bytes);
	if (!restore_buffer) {
		N_Ef(restore_struct_alloc_failed, "Failed to allocate restore buffer size=@SIZE_T", restore_size_bytes);
		goto out;
	}

	backup_fd = NNVMEIBT_OPEN_READ(trace_restore_struct_open, filepath, 1);
	if (backup_fd < 0) {
		N_Ef(restore_struct_open_failed, "Cannot open @STR", filepath);
		goto out;
	}

	/* Use TOMA I/O helper for robust reading (handles EINTR, partial I/O) */
	if (NNVMEIBT_PREAD_ATOMIC(trace_restore_struct_read, backup_fd, restore_buffer, restore_size_bytes, 0, 1) != (ssize_t)restore_size_bytes) {
		N_Ef(restore_struct_read_failed, "Cannot read @STR", filepath);
		goto out;
	}

	if (NNVMEIBT_PWRITE(trace_restore_struct_write, disk_fd, restore_buffer, restore_size_bytes, pbyte_start, 0) != (ssize_t)restore_size_bytes) {
		N_Ef(restore_struct_write_failed, "Cannot write structure to device pba=@ZX", pba_start);
		goto out;
	}

	rv = 0;

out:
	if (backup_fd >= 0) {
		close(backup_fd);
	}
	NNVMEIBT_BM_FREE(trace_restore_struct_buf_free, restore_buffer);
	return rv;
}

/**
 * Execute RESTORE_BINARY action
 * Restores device from modular binary backup (manifest + hidden files)
 * Manifest file format: JSON with list of structures and their file paths
 */
static int execute_restore_binary(int disk_fd, struct gpt_util_config *config)
{
	int											rv = -1;
	struct mm_json_elem							*json_root = NULL;
	struct mm_json_elem							*structures_array = NULL;
	const char									*device_path_in_manifest = NULL;
	const char									*manifest_serial = NULL;
	const char									*manifest_version = NULL;
	int											block_size_in_manifest = 0;
	int											n_structures_restored = 0;
	uint64_t									total_bytes_restored = 0;

	fprintf(stdout, "\n=== Restoring from Modular Binary Backup ===\n");
	fprintf(stdout, "Manifest file: %s\n", config->restore_binary_file);
	fprintf(stdout, "Device: %s\n", config->device_path);
	fprintf(stdout, "Mode: %s\n", config->write_mode ? COL_YELLOW "WRITE" COL_RESET : COL_YELLOW "DRY-RUN" COL_RESET " (use --write to restore)");

	/* Load and parse manifest file */
	json_root = load_json_file_or_fail(config->restore_binary_file);
	if (!json_root) {
		rv = -1;
		goto out;
	}

	/* Extract metadata from manifest */
	device_path_in_manifest = json_get_dict_str(json_root, "device_path", NULL);
	block_size_in_manifest = (int)json_get_dict_num(json_root, "block_size", 0);
	structures_array = json_get_dict_value(json_root, "structures");

	if (!device_path_in_manifest || block_size_in_manifest == 0 || !structures_array) {
		N_Ef(restore_manifest_missing_fields, "Manifest missing required fields @STR", config->restore_binary_file);
		fprintf(stderr, COL_RED_BOLD "ERROR: Invalid manifest - missing required fields" COL_RESET "\n");
		goto out;
	}

	/* Validate gpt_util version compatibility */
	manifest_version = json_get_dict_str(json_root, "gpt_util_version", NULL);
	if (!manifest_version) {
		N_Ef(restore_manifest_no_version, "Manifest missing gpt_util_version field @STR", config->restore_binary_file);
		rv = -1;
		goto out;
	} else {
		fprintf(stdout, "Backup version: %s (current: %s)\n", manifest_version, GPT_UTIL_VERSION);
		if (strcmp(manifest_version, GPT_UTIL_VERSION) != 0) {
			N_Ef(restore_manifest_version_mismatch, "Version mismatch: manifest=@STR current=@STR file=@STR",
					manifest_version, GPT_UTIL_VERSION, config->restore_binary_file);
			fprintf(stderr, COL_RED_BOLD "ERROR: Backup was created by different gpt_util version. Exiting." COL_RESET "\n");
			rv = -1;
			goto out;
		}
	}

	if (structures_array->type != JSON_E_ARRAY) {
		N_Ef(restore_manifest_bad_structures, "Manifest structures is not an array @STR", config->restore_binary_file);
		fprintf(stderr, COL_RED_BOLD "ERROR: Invalid manifest - structures must be array" COL_RESET "\n");
		goto out;
	}

	/* Validate block size matches */
	if (block_size_in_manifest != config->pblk_size) {
		N_Ef(restore_block_size_mismatch, "Block size mismatch: manifest=@INT device=@INT",
			 block_size_in_manifest, config->pblk_size);
		fprintf(stderr, COL_RED_BOLD "ERROR: Block size mismatch (manifest=%d, device=%d)" COL_RESET "\n",
				block_size_in_manifest, config->pblk_size);
		goto out;
	}

	fprintf(stdout, "\nManifest Info:\n");
	fprintf(stdout, "  Original device: %s\n", device_path_in_manifest);
	fprintf(stdout, "  Block size: %d\n", block_size_in_manifest);
	fprintf(stdout, "  Structures: %d\n", structures_array->array.len);

	/* NVMesh devices have at least 10 structures (+ variable segment metadata control blocks) */
	if (structures_array->array.len < 10) {
		N_Ef(restore_wrong_structure_count, "Invalid structure count: minimum=10 actual=@INT", structures_array->array.len);
		fprintf(stderr, COL_RED_BOLD "ERROR: Manifest has too few structures!" COL_RESET "\n");
		fprintf(stderr, "  Minimum: 10 (1 MBR + 4 Main GPT + 4 Metadata GPT + 1 disk_metadata)\n");
		fprintf(stderr, "  Actual: %d\n", structures_array->array.len);
		fprintf(stderr, "  This backup is incomplete or corrupt.\n");
		rv = -1;
		goto out;
	}
	if (structures_array->array.len > 10) {
		N_Tf(restore_has_seg_md_ctrls, "Backup includes @INT segment metadata control blocks", structures_array->array.len - 10);
		fprintf(stdout, "  Note: Backup includes %d segment metadata control blocks (in addition to 10 base structures)\n",
				structures_array->array.len - 10);

		/* Validate that extra structures (beyond first 10) are segment metadata control blocks */
		for (int i = 10; i < structures_array->array.len; i++) {
			struct mm_json_elem	*structure_elem = structures_array->array.elements[i];
			const char			*name;
			uint64_t			n_blocks;

			if (structure_elem->type != JSON_E_DICT) {
				continue;		// Will be caught in validation loop below
			}

			name = json_get_dict_str(structure_elem, "name", NULL);
			n_blocks = (uint64_t)json_get_dict_num(structure_elem, "n_blocks", 0);

			/* Validate: name starts with "seg_md_ctrl_", n_blocks == 1 */
			if (!name || strncmp(name, "seg_md_ctrl_", 12) != 0) {
				N_Ef(restore_invalid_extra_structure, "Structure @INT has invalid name (expected seg_md_ctrl_*): @STR",
					 i, name ? name : "(null)");
				fprintf(stderr, COL_RED_BOLD "ERROR: Invalid structure in manifest!" COL_RESET "\n");
				fprintf(stderr, "  Structure %d: name='%s'\n", i, name ? name : "(null)");
				fprintf(stderr, "  Expected: Structures beyond first 10 must be segment metadata control blocks (seg_md_ctrl_*).\n");
				rv = -1;
				goto out;
			}
			if (n_blocks != 1) {
				N_Ef(restore_invalid_seg_md_size, "Segment metadata structure @INT has invalid size: @INT blocks (expected 1)",
					 i, (int)n_blocks);
				fprintf(stderr, COL_RED_BOLD "ERROR: Invalid segment metadata control block size!" COL_RESET "\n");
				fprintf(stderr, "  Structure %d (%s): %lu blocks (expected 1)\n", i, name, n_blocks);
				rv = -1;
				goto out;
			}
		}
	}

	/* Validation 1: Verify all structure files exist and have correct sizes */
	fprintf(stdout, "\nValidating backup files...\n");
	for (int i = 0; i < structures_array->array.len; i++) {
		struct mm_json_elem		*structure_elem = structures_array->array.elements[i];
		const char				*name;
		const char				*file;
		uint64_t				n_blocks;
		uint64_t				expected_size;
		struct stat				st;

		if (structure_elem->type != JSON_E_DICT) {
			N_Ef(restore_validate1_not_dict, "Structure @INT is not a dict", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Corrupt manifest - structure %d is not a dict" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		name = json_get_dict_str(structure_elem, "name", NULL);
		file = json_get_dict_str(structure_elem, "file", NULL);
		n_blocks = (uint64_t)json_get_dict_num(structure_elem, "n_blocks", 0);

		if (!name || !file || n_blocks == 0) {
			N_Ef(restore_validate1_missing_field, "Structure @INT missing required field (name/file/n_blocks)", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Corrupt manifest - structure %d has missing fields" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		/* Guard against integer overflow in size calculation */
		if (n_blocks > UINT64_MAX / config->pblk_size) {
			N_Ef(restore_size_overflow, "Size calculation overflow: n_blocks=@ZX pblk_size=@INT", n_blocks, config->pblk_size);
			fprintf(stderr, COL_RED_BOLD "ERROR: Structure size overflow in manifest!" COL_RESET "\n");
			fprintf(stderr, "  Structure: %s\n", name);
			rv = -1;
			goto out;
		}

		expected_size = n_blocks * config->pblk_size;

		/* Check file exists */
		if (stat(file, &st) < 0) {
			N_Ef(restore_file_missing, "Structure file missing: @STR", file);
			fprintf(stderr, COL_RED_BOLD "ERROR: Backup file missing: %s" COL_RESET "\n", file);
			fprintf(stderr, "  Structure: %s\n", name);
			rv = -1;
			goto out;
		}

		/* Validate file size */
		if ((uint64_t)st.st_size != expected_size) {
			N_Ef(restore_file_size_wrong, "Structure file size mismatch: @STR expected=@SIZE_T actual=@SIZE_T",
				 file, expected_size, (uint64_t)st.st_size);
			fprintf(stderr, COL_RED_BOLD "ERROR: Backup file size mismatch: %s" COL_RESET "\n", file);
			fprintf(stderr, "  Expected: %lu bytes\n", expected_size);
			fprintf(stderr, "  Actual: %lu bytes\n", (uint64_t)st.st_size);
			rv = -1;
			goto out;
		}

		N_Tf(restore_file_size_match, "File size match: @STR size=@SIZE_T", name, (uint64_t)st.st_size);
	}

	/* Validation 2: Verify serial number matches device (fail-closed) */
	fprintf(stdout, "\nValidating device serial number...\n");
	manifest_serial = json_get_dict_str(json_root, "controller_serial_num", NULL);
	if (validate_serial_number_match(disk_fd, config, manifest_serial, "manifest") < 0) {
		rv = -1;
		goto out;
	}

	/* Validation 3: Verify PBA boundaries don't exceed device size */
	fprintf(stdout, "\nValidating PBA boundaries...\n");
	for (int i = 0; i < structures_array->array.len; i++) {
		struct mm_json_elem		*structure_elem;
		const char				*name;
		uint64_t				pba_start;
		uint64_t				n_blocks;
		uint64_t				pba_end;

		structure_elem = structures_array->array.elements[i];

		if (structure_elem->type != JSON_E_DICT) {
			N_Ef(restore_validate3_not_dict, "Structure @INT is not a dict", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Corrupt manifest - structure %d is not a dict" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		name = json_get_dict_str(structure_elem, "name", NULL);
		pba_start = (uint64_t)json_get_dict_num(structure_elem, "pba_start", -1);
		n_blocks = (uint64_t)json_get_dict_num(structure_elem, "n_blocks", 0);

		if (!name || pba_start == (uint64_t)-1 || n_blocks == 0) {
			N_Ef(restore_validate3_missing_field, "Structure @INT missing required field (name/pba_start/n_blocks)", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Corrupt manifest - structure %d has missing fields" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		/* Guard against integer overflow in PBA calculation */
		if (pba_start > UINT64_MAX - n_blocks) {
			N_Ef(restore_pba_overflow_calc, "PBA calculation overflow: pba_start=@ZX n_blocks=@ZX", pba_start, n_blocks);
			fprintf(stderr, COL_RED_BOLD "ERROR: PBA range overflow in manifest!" COL_RESET "\n");
			fprintf(stderr, "  Structure: %s\n", name);
			rv = -1;
			goto out;
		}

		pba_end = pba_start + n_blocks - 1;

		/* Check against device boundaries */
		if (pba_end > config->pba_e) {
			N_Ef(restore_pba_out_of_bounds, "Structure @STR exceeds device: pba_end=@ZX device_pba_e=@ZX",
				 name, pba_end, config->pba_e);
			fprintf(stderr, COL_RED_BOLD "ERROR: Structure would overwrite beyond device end!" COL_RESET "\n");
			fprintf(stderr, "  Structure: %s\n", name);
			fprintf(stderr, "  PBA range: %lu-%lu\n", pba_start, pba_end);
			fprintf(stderr, "  Device end: %lu\n", config->pba_e);
			fprintf(stderr, "  This backup is from a larger device!\n");
			rv = -1;
			goto out;
		}
	}
	N_Tf(restore_pba_boundaries_match, "All structures within device boundaries (device PBA end: @PBA_E)", config->pba_e);

	/* Dry-run mode: Stop here, don't write */
	if (!config->write_mode) {
		uint64_t	total_bytes = 0;

		/* Calculate accurate total bytes from manifest */
		for (int j = 0; j < structures_array->array.len; j++) {
			struct mm_json_elem	*structure_elem = structures_array->array.elements[j];
			uint64_t			n_blocks;

			if (structure_elem->type == JSON_E_DICT) {
				n_blocks = (uint64_t)json_get_dict_num(structure_elem, "n_blocks", 0);
				total_bytes += n_blocks * config->pblk_size;
			}
		}

		fprintf(stdout, "\n" COL_GREEN "=== Dry-Run Complete ===" COL_RESET "\n");
		fprintf(stdout, "Would restore %d structures (%lu bytes total)\n", structures_array->array.len, total_bytes);
		fprintf(stdout, COL_YELLOW "Use --write to apply changes." COL_RESET "\n");
		rv = 0;
		goto out;
	}

	/* Confirm before restore */
	if (!validate_and_confirm_write(config, "Restore from modular backup")) {
		fprintf(stdout, "Restore cancelled.\n");
		rv = 0;		/* User cancelled - not an error */
		goto out;
	}

	fprintf(stdout, "\n" COL_YELLOW "=== Writing Structures to Device ===" COL_RESET "\n");

	/* Restore each structure */
	for (int i = 0; i < structures_array->array.len; i++) {
		struct mm_json_elem		*structure_elem = structures_array->array.elements[i];
		const char				*name;
		const char				*file;
		uint64_t				pba_start;
		uint64_t				n_blocks;

		if (structure_elem->type != JSON_E_DICT) {
			N_Ef(restore_loop_not_dict, "Structure @INT is not a dict (validation should have caught this)", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Internal error - structure %d is not a dict" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		name = json_get_dict_str(structure_elem, "name", NULL);
		file = json_get_dict_str(structure_elem, "file", NULL);
		pba_start = (uint64_t)json_get_dict_num(structure_elem, "pba_start", -1);
		n_blocks = (uint64_t)json_get_dict_num(structure_elem, "n_blocks", 0);

		if (!name || !file || pba_start == (uint64_t)-1 || n_blocks == 0) {
			N_Ef(restore_loop_missing_field, "Structure @INT missing required field (validation should have caught this)", i);
			fprintf(stderr, COL_RED_BOLD "ERROR: Internal error - structure %d has missing fields" COL_RESET "\n", i);
			rv = -1;
			goto out;
		}

		fprintf(stdout, "  [%d/%d] %s (PBA %lu, %lu blocks)...\n",
				i + 1, structures_array->array.len, name, pba_start, n_blocks);

		if (restore_structure(disk_fd, file, pba_start, n_blocks, config->pblk_size) < 0) {
			N_Ef(restore_structure_failed, "Failed to restore structure @STR from @STR", name, file);
			fprintf(stderr, COL_RED_BOLD "ERROR: Failed to restore %s" COL_RESET "\n", name);
			/* Warn about partial restore (restore is NOT atomic) */
			fprintf(stderr, "  Structures restored before failure: %d/%d\n", n_structures_restored, structures_array->array.len);
			fprintf(stderr, COL_YELLOW "  WARNING: Device is in INCONSISTENT state!" COL_RESET "\n");
			fprintf(stderr, "  Some structures from backup, some original.\n");
			goto out;
		}

		n_structures_restored++;
		total_bytes_restored += n_blocks * config->pblk_size;
	}

	/* All structures from manifest must be restored successfully */
	if (n_structures_restored != structures_array->array.len) {
		N_Ef(restore_incomplete, "Incomplete restore: expected=@INT actual=@INT", structures_array->array.len, n_structures_restored);
		fprintf(stderr, COL_RED_BOLD "ERROR: Only %d/%d structures were restored!" COL_RESET "\n",
				n_structures_restored, structures_array->array.len);
		fprintf(stderr, "  Device is in INCONSISTENT state!\n");
		fprintf(stderr, "  Some structures from backup, some original.\n");
		rv = -1;
		goto out;
	}

	/* Check fsync return value to ensure changes are durable */
	if (fsync(disk_fd) < 0) {
		N_Ef(restore_fsync_failed, "Failed to sync device @AUTO_ERRNO");
		fprintf(stderr, COL_RED_BOLD "ERROR: Failed to sync changes to device" COL_RESET "\n");
		fprintf(stderr, "  Restore may not be durable!\n");
		rv = -1;
		goto out;
	}

	N_IMf(restore_success, "Device restored from modular backup: dev=@STR manifest=@STR structures=@INT bytes=@SIZE_T",
		  config->device_path, config->restore_binary_file, n_structures_restored, total_bytes_restored);

	fprintf(stdout, "\n" COL_GREEN "=== Device Restored Successfully ===" COL_RESET "\n");
	fprintf(stdout, "Device: %s\n", config->device_path);
	fprintf(stdout, "Structures restored: %d/%d\n", n_structures_restored, structures_array->array.len);
	fprintf(stdout, "Total bytes: %lu\n", total_bytes_restored);

	rv = 0;

out:
	if (json_root) {
		nvmeibt_mm_json_free_kv_tree(json_root);
	}
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
 * Run GPT utility operation
 * Uses 4-phase architecture: parse, validate, setup, execute
 * @param is_self_test: true if running in self-test mode
 */
static int run_gpt_util_op(int argc, char *argv[], BOOL is_self_test)
{
	int							rv = 1;
	int							disk_fd = -1;
	struct gpt_util_config		config;

	// Initialize config with defaults
	memset(&config, 0, sizeof(config));
	config.action = ACTION_DISPLAY_GPT;		// Default action
	config.gpt_copy_option = GPT_COPY_OPTION_PRIMARY;
	config.o_direct_mode = O_DIRECT_AUTO;	// Auto-detect O_DIRECT based on file type
	config.is_self_test = is_self_test;		// Allow mock serials in self-test mode

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

	case ACTION_CHECK_NVMESH:
		rv = execute_check_nvmesh(disk_fd, &config);
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

	case ACTION_RESTORE_BINARY:
		rv = execute_restore_binary(disk_fd, &config);
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

	release_toma_lock_if_acquired();

	return rv;
}

/**
 * Self-test wrapper for run_gpt_util_op (keeps production function static)
 * Exposed API for self-test framework to invoke gpt_util operations
 */
int SELF_TEST_run_gpt_util_op(int argc, char *argv[])
{
	return run_gpt_util_op(argc, argv, true);		// Self-test mode
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
	BOOL		is_self_test = false;
	BOOL		quiet_mode = false;
	const char	*test_selection = NULL;		// NULL = run all tests
	struct nvmeibt_km_comm_params par;

	// Quick check for --self-test and --quiet flags (before full parsing)
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--self-test") == 0) {
			is_self_test = true;
			// Check if next argument is a test selection (number, range, or list)
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				test_selection = argv[i + 1];
			}
		} else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
			quiet_mode = true;
		}
	}

	// Start trace pollers (gpt_util is a utility, routes traces properly)
	nvmeibt_start_all_trace_pollers(true);

	// Buffer Manager
	if (nvmeibt_bm_create()) {
		N_Ef(gpt_util_bm_create_failed, "Failed to create buffer manager");
		return 1;
	}

	// Print version banner
	print_version_banner();

	// Conenct to server in passive mode
	memset(&par, 0, sizeof(par));
	#ifdef TOMA_USE_USER_SPACE_SERVER_API
		par.use_user_space_api = true;
	#endif
	par.use_only_passive_util_mode = true;
	if (nvmeib_srvr_api_lib_create(&par) < 0) {
		return 1;
	}

	// Dispatch to appropriate mode
	if (is_self_test) {
		rv = run_self_test(test_selection, quiet_mode);
	} else {
		rv = run_gpt_util_op(argc, argv, false);
	}

	nvmeib_srvr_api_lib_destroy();
	nvmeibt_bm_destroy();
	nvmeibt_join_all_trace_pollers();		// Clean shutdown of trace threads; beyond this point, no more traces

	return rv;
}

