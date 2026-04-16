/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBR_DS_METADATA_H
#define NVMEIBR_DS_METADATA_H

#include "nvmeibt_common.h"

/*
 * The sizeof metadata magic string is 59 bytes (including '\0').
 * For nicer alignment we reserve 64 bytes for the string in the header.
 */
#define DS_METADATA_MAGIC_STR_SIZE	64

#define NUM_TOPOLOGY_PERSISTENCY_4KBLKS			2
#define NUM_CONFIGURATION_PERSISTENCY_4KBLKS	60
#define NUM_PERSIST_CTRL_4KBLKS					1

#define DS_METADATA_NUM_CTRL_4KBLKS 		(1)
#if (DS_METADATA_NUM_CTRL_4KBLKS != 1)
#	error Disk segment expects single metadata control block
#endif

#define CTRL_BLK_RELATIVE_OFFSET_BYTES	0
#define TOPO_0_RELATIVE_OFFSET_BYTES			(CTRL_BLK_RELATIVE_OFFSET_BYTES + (DS_METADATA_NUM_CTRL_4KBLKS * SECTOR_SIZE))
#define TOPO_1_RELATIVE_OFFSET_BYTES			(TOPO_0_RELATIVE_OFFSET_BYTES   + (NUM_TOPOLOGY_PERSISTENCY_4KBLKS * SECTOR_SIZE))
#define CONFIG_0_RELATIVE_OFFSET_BYTES			(TOPO_1_RELATIVE_OFFSET_BYTES   + (NUM_TOPOLOGY_PERSISTENCY_4KBLKS * SECTOR_SIZE))
#define CONFIG_1_RELATIVE_OFFSET_BYTES			(CONFIG_0_RELATIVE_OFFSET_BYTES + (NUM_CONFIGURATION_PERSISTENCY_4KBLKS * SECTOR_SIZE))
#define LOCKS_TABLE_RELATIVE_OFFSET_BYTES		(CONFIG_1_RELATIVE_OFFSET_BYTES + (NUM_CONFIGURATION_PERSISTENCY_4KBLKS * SECTOR_SIZE))
#define FIXED_PART_METADATA_SIZE				LOCKS_TABLE_RELATIVE_OFFSET_BYTES

/*
 * The metadata header is _stable_ forever and should _never_ change, so all
 * versions can expect it as is. The remaining fields may be version specific.
 * The header is nicely aligned to 128 bytes.
 */
struct nvmeibt_disk_segment_metadata_hdr {
	char 						magic_str[DS_METADATA_MAGIC_STR_SIZE];
	unsigned int				encoding_ver;					// TOMA_ENCODING_VER when this metadata was written
	unsigned int 				seg_metadata_version;
	union nvmeib_uuid 			mgmt_db_uuid;
	char						__unused[40];					// must be 0 filled
} __attribute__((packed));

struct nvmeibt_seg_active_metadata_ctrl {
	struct nvmeibt_disk_segment_metadata_hdr	header;
	union nvmeib_uuid							disk_segment_uuid;
	int64_t										save_timespec_tv_sec;
	int64_t										UNUSED__Before_3_2_this_and_the_prev_field_were_struct_timecal__16_bytes_together_on_all_machines;
	uint64_t									locks_table_pbyte_s;
	u64                                         reservation_mode_version;
	int											active_praid_version_major;		// Used for validating dirty&stale ALIVE_STABLE shutdown
	int											active_praid_version_minor;		// Used for validating dirty&stale ALIVE_STABLE shutdown
	int											committed_praid_config_version;	// Unused
	//
	BOOL 										is_current_shutdown_clean;
	signed char 								is_written_on_disk;			// enum nvmeibt_dseg_md_ctrl_blk_status
	BOOL										reserved_was_is_zeroed_after_delete;
	char 										hostname[NVMEIB_HOST_NAME_LEN];
	uint64_t									metadata_pbyte_s;
	uint64_t									n_blksets_scrubbed;
	char										unused[32];
	uint32_t									metadata_ctrl_crc32;
	uint32_t									locks_table_crc32;
	char										__zeroed_filler_till_4K__[0];	// must be 0 filled
	char										__END_of_filler_for_4K__[0]	__attribute__((aligned(4096)));	// Even if pblk_size is 512, the srvr accepts only 4K
} __attribute__((packed, aligned(4096)));

_Static_assert(sizeof(struct nvmeibt_seg_active_metadata_ctrl) == 4096, "sizeof(struct nvmeibt_seg_active_metadata_ctrl) != 4096");

enum nvmeibt_dseg_md_ctrl_blk_status {				 // Crucial before we can apply topo to this segment
  //NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ERROR       =-1, // Not in persistancy, and not in RAM (can even try to write it to persistancy)
	NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK = 0, // Not in persistancy, default value in RAM (allocated and initialized to empty default values)
	NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_REQ_UPGRADE = 1,
	NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID = 2, // On disk, valid md version, maybe old topology (not updated on every praid change)
};

struct seg_persistency_write_params {
	struct netlink_io_context					*nl_ctx;
	int											fd;
	int											pblk_size;
	struct nvmeibt_seg_active_metadata_ctrl		*live_seg_persistent_metadata;
	struct nvmeibt_seg_active					*seg_active;
	uint64_t									num_blksets;
	const char									*dev_file_name;
	uint64_t									seg_metadata_pba_s;
};

struct nvmeibt_seg_active;
struct nvmeibt_disk_segment;
struct nvmeibt_disk_segment_config;
struct netlink_io_context;
struct nvmeibt_local_disk;

/* Store the disk segment state to its metadata in disk. It is assumed that this function is called after IO had been halted namely no locks will be taken or dropped for this segments blksets*/
void nvmeibt_ds_metadata_store_on_shutdown(struct nvmeibt_seg_active *seg_active);

/* Initializes the metadata control structure. */
int  nvmeibt_seg_active_metadata_ctrl_init(struct nvmeibt_seg_active *seg_active);
void nvmeibt_seg_active_del_ptr_to_locks_tbl(struct nvmeibt_seg_active *);

int nvmeibt_ds_metadata_validate_and_upgrade_persistent_metadata_as_needed(struct nvmeibt_seg_active_metadata_ctrl *persistent_metadata, const union nvmeib_uuid *seg_uuid);

/* Write the disk segment metadata control block to disk. */
/* Return the size of the disk segments metadata.*/
uint64_t nvmeibt_ds_metadata_size_in_disk_pblks(struct nvmeibt_disk_segment *disk_segment);

void nvmeibt_ds_metadata_set_error_injection_md_write_pause(int value);
int nvmeibt_ds_metadata_ctrl_blk_write(struct seg_persistency_write_params *write_params);

/* Restore the RAM (locks, TxID, dbits) from persistency (metadata of segment), Or store from RAM to persistrncy */
int nvmeibt_ds_metadata_locks_table_restore(struct nvmeibt_seg_active *seg_active, bool *is_stale_rebuild_required);

/* Read segment metadata control block from disk (for gpt_util and diagnostics) */
int nvmeibt_ds_metadata_ctrl_blk_read(struct netlink_io_context *nl_ctx, int fd,
									  int pblk_size, uint64_t pbyte_s,
									  struct nvmeibt_seg_active_metadata_ctrl *seg_md_ctrl);

#endif //NVMEIBR_DS_METADATA_H
