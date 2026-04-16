/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "local/ram/nvmeibt_ds_blkset_entries.h"

enum PERSISTENCY_SECTION_TYPE {
	PERSIST_SECTION_DIRTY_BITS_v1_2 			= (0x1 << 0),
	PERSIST_SECTION_STALE_LOCKS_v1_2 			= (0x1 << 1),
	PERSIST_SECTION_LOCKS_TABLE 				= (0x1 << 2),			// R1/EC D+P uses only P+1 entries for instead of D+P.
	PERSIST_SECTION_TOPO_SLOT_NO_0 				= (0x1 << 3),			// Unused
	PERSIST_SECTION_TOPO_SLOT_NO_1 				= (0x1 << 4),			// Unused
	PERSIST_SECTION_CONFIG_SLOT_NO_0 			= (0x1 << 5),
	PERSIST_SECTION_CONFIG_SLOT_NO_1 			= (0x1 << 6),
	PERSIST_SECTION_PERSIST_CTRL_BLK			= (0x1 << 7),			// Stores (struct nvmeibt_disk_segment_metadata_ctrl)
};

#define NDUMP_SEGMENT_METADATA_CTRL_STRUCT(name, stc_ptr, prefix_str) \
		N_Tf(name, "@STR magic_str='@MAGIC_STR' seg_id=@UUID_LE is_shutdown_clean=@IS_SHUTDOWN_CLEAN is_written=@IS_WRITTEN praid_version=@PRAID_VERSION.@PRAID_VERSION " \
			"hostname=@HOSTNAME save_time_sec=@INT64_TD sw_ver=@SW_VER seg_metadata_ver=@SEG_METADATA_VER metadata_pbyte_s=@UINT64_TX locks_table_s_b=@LOCKS_TABLE_S_B "	\
		    "mgmt_db_uuid=@UUID_LE",	\
			prefix_str,											\
			(stc_ptr)->header.magic_str,						\
		   &(stc_ptr)->disk_segment_uuid,						\
			(stc_ptr)->is_current_shutdown_clean,				\
			(stc_ptr)->is_written_on_disk,						\
			(stc_ptr)->active_praid_version_major,				\
			(stc_ptr)->active_praid_version_minor,				\
			(stc_ptr)->hostname,								\
			(stc_ptr)->save_timespec_tv_sec,					\
			(stc_ptr)->header.encoding_ver,						\
			(stc_ptr)->header.seg_metadata_version,				\
			(stc_ptr)->metadata_pbyte_s,						\
			(stc_ptr)->locks_table_pbyte_s,						\
		   &(stc_ptr)->header.mgmt_db_uuid)

#include "toma/disk_seg_meta_data/nvmeibt_ds_md_old_versions_code.h"

static int error_injection_md_write_pause = 0;

void nvmeibt_seg_active_del_ptr_to_locks_tbl(struct nvmeibt_seg_active *seg_active)
{
	NFIN;
	if (seg_active) {
		if (!seg_active->mmap_locks_tbl) {
			N_Tf(rii98c5, "seg=@UUID_8 locks table is NOT mmapped", nvmeibt_seg_active_UUID_8(seg_active));
		}
		seg_active->mmap_locks_tbl = NULL;
	}
	NFOUT;
}

/* Calculate number of blks reserved in segments metadata for locks table */
static uint64_t calc_n_allocated_4kblks_for_locks_ents_by_n_blksets(uint64_t n_blksets)
{
	uint64_t n_ents_in_4kblk = SECTOR_SIZE / NVMEIB_LOCK_BLKSET_ENTRY_SIZE;
	uint64_t n_total_ents_4kblk = divroundup(n_blksets, n_ents_in_4kblk);

	BUILD_BUG_ON(SECTOR_SIZE != ((SECTOR_SIZE/NVMEIB_LOCK_BLKSET_ENTRY_SIZE)*NVMEIB_LOCK_BLKSET_ENTRY_SIZE));	// Entries must fin nicely in block on disk
	N_Tf(trace_05_toma_dsmd, "n_total_ents_4kblk=@N_TOTAL_ENTS_4KBLK", n_total_ents_4kblk);

	return n_total_ents_4kblk;
}

static uint64_t calc_n_allocated_locks_ents_4kblks(struct nvmeibt_disk_segment *disk_segment)
{
	return calc_n_allocated_4kblks_for_locks_ents_by_n_blksets(num_blksets_in_disk_segment(disk_segment));
}

/*
 * Calculate the offset (in bytes) in segment metadata of a given section.
 * Return (uint64) -1LL to indicate error.
 */
static uint64_t ds_metadata_get_section_start_byte_abs(
				enum PERSISTENCY_SECTION_TYPE section,
				const struct nvmeibt_seg_active_metadata_ctrl *mdctrl,
				const struct nvmeibt_seg_active *seg_active,
				uint64_t n_blksets,
				uint64_t metadata_start_byte_abs,
				uint64_t *target_buff_size)
{
	uint64_t	section_start_byte_abs = (uint64_t) -1LL;
	uint64_t	n_4kblks;

	NFIN;

	*target_buff_size = 0;

	switch (section) {
	case PERSIST_SECTION_LOCKS_TABLE:
		n_4kblks = calc_n_allocated_4kblks_for_locks_ents_by_n_blksets(n_blksets);
		if (mdctrl->locks_table_pbyte_s > 0) {
			section_start_byte_abs = mdctrl->locks_table_pbyte_s;
		} else {
			N_Ef(trace_11_toma_dsmd, "No locks_table_start_byte initialized for seg:@UUID_LE", nvmeibt_seg_active_UUID(seg_active));
			goto out;
		}
		break;
	case PERSIST_SECTION_TOPO_SLOT_NO_0:
		section_start_byte_abs = metadata_start_byte_abs + TOPO_0_RELATIVE_OFFSET_BYTES;
		n_4kblks = NUM_TOPOLOGY_PERSISTENCY_4KBLKS;
		break;
	case PERSIST_SECTION_TOPO_SLOT_NO_1:
		section_start_byte_abs = metadata_start_byte_abs + TOPO_1_RELATIVE_OFFSET_BYTES;
		n_4kblks = NUM_TOPOLOGY_PERSISTENCY_4KBLKS;
		break;
	case PERSIST_SECTION_CONFIG_SLOT_NO_0:
		section_start_byte_abs = metadata_start_byte_abs + CONFIG_0_RELATIVE_OFFSET_BYTES;
		n_4kblks = NUM_CONFIGURATION_PERSISTENCY_4KBLKS;
		break;
	case PERSIST_SECTION_CONFIG_SLOT_NO_1:
		section_start_byte_abs = metadata_start_byte_abs + CONFIG_1_RELATIVE_OFFSET_BYTES;
		n_4kblks = NUM_CONFIGURATION_PERSISTENCY_4KBLKS;
		break;
	case PERSIST_SECTION_PERSIST_CTRL_BLK:
		section_start_byte_abs = metadata_start_byte_abs + CTRL_BLK_RELATIVE_OFFSET_BYTES;
		n_4kblks = NUM_PERSIST_CTRL_4KBLKS;
		break;
	default:
		N_Ef(trace_12_toma_dsmd, "ERROR: unknown section type=@SECTION", section);
		goto out;
	}

	*target_buff_size = n_4kblks * SECTOR_SIZE;

out:
	NFOUT;
	return section_start_byte_abs;
}

/*
 * Reads a buffer from the persistent storage from the given section.
 * Returns 0 on success, -1 upon error(including an error if buffer is too small)
 */
static int ds_metadata_read_buff(
				const struct nvmeibt_seg_active		*seg_active,
				char *buff,
				size_t buff_size,
				enum PERSISTENCY_SECTION_TYPE section,
				int fd,
				uint64_t metadata_start_byte_abs,
				uint64_t read_start_byte_abs)
{
	struct nvmeibt_disk_segment		*disk_segment;
	int 							rv = -1;
	uint64_t						section_start_byte_abs = 0;
	uint64_t						target_buff_size = 0;

	NFIN;
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	N_Tf(wwy7639, "seg=@UUID_8 buf_size=@BUF_SIZE section=@SECTION fd=@FD read_start_byte_abs=@SECTION_START_BYTE",
		nvmeibt_seg_UUID_8(disk_segment), buff_size, section, fd, read_start_byte_abs);
	if (fd < 0) {
		N_Ef(ggko09d, "Invalid file descriptor passed @FD, @AUTO_ERRNO", fd);
		goto out;
	}

	section_start_byte_abs = ds_metadata_get_section_start_byte_abs(
		section, seg_active->persistent_metadata, seg_active,
		num_blksets_in_disk_segment(disk_segment), metadata_start_byte_abs, &target_buff_size);

	if (section_start_byte_abs != read_start_byte_abs) {
		N_Ef(fju8734, "section_start[b]=@ZU != read_start[b]=@ZU", section_start_byte_abs, read_start_byte_abs);
		goto out;
	}
	if (target_buff_size > buff_size) {
		N_Ef(ff99g26, "ERROR: target buffer too small. buff_size=@ZU, needed=@ZU, section=@SECTION", buff_size, target_buff_size, section);
		goto out;
	}
	if (NNVMEIBT_PREAD(trace_25_toma_dsmd, fd, buff, buff_size, section_start_byte_abs, true) < 0) {
		N_Wf(go09t86, "Failed to read data of size @ZU from offset_bytes @ZU", buff_size, section_start_byte_abs);
		goto out;
	}
	if (section == PERSIST_SECTION_PERSIST_CTRL_BLK) {
		NTOMA_ASSERT(aau8782, ((void*)seg_active->persistent_metadata == (void*)buff), "Incorrect usage of control block read");
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

/*
 * Writes a buffer from the persistent storage to the given section.
 * Returns 0, -1 upon error (including if the buffer is to large)
 */
static int ds_metadata_write_buff(
				struct seg_persistency_write_params *write_params,
				const void *buff,
				size_t buff_size,
				enum PERSISTENCY_SECTION_TYPE section)
{
	int rv = -1;
	uint64_t section_start_byte_abs = 0;
	uint64_t target_buff_size = 0;
	uint64_t seg_metadata_pbyte_s;
	int pblk_size = write_params->pblk_size;

	NFIN;

	buff_size = roundup(buff_size, pblk_size);
	seg_metadata_pbyte_s = write_params->seg_metadata_pba_s * write_params->pblk_size;
	if (seg_metadata_pbyte_s == 0) {
		nvmeibt_abort(ES_FATAL);
	}

	section_start_byte_abs = ds_metadata_get_section_start_byte_abs(
					section,
					write_params->live_seg_persistent_metadata,
					write_params->seg_active,
					write_params->num_blksets,
					seg_metadata_pbyte_s,
					&target_buff_size);

	N_Tf(trace_31_toma_dsmd, "buf=@BUF buf_size=@ZU section=@SECTION fd=@FD section_start_byte=@ZU",
		(void *)buff, buff_size, section, write_params->fd, section_start_byte_abs);

	if (section_start_byte_abs == (uint64_t) -1LL) {
		N_Ef(trace_32_toma_dsmd, "ERROR: failed to compute offset: section=@SECTION", section);
		goto out;
	}

	if (target_buff_size < buff_size) {
		N_Ef(trace_33_toma_dsmd, "ERROR: source buffer too large: buff_size=@BUFF_SIZE, disk_space=@DISK_SPACE, section=@SECTION",
			buff_size, target_buff_size, section);
		goto out;
	}

	if (NNVMEIBT_PWRITE(trace_34_toma_dsmd, write_params->fd, buff, buff_size, section_start_byte_abs, pblk_size*4) < 0) {
		N_Wf(trace_35_toma_dsmd, "Failed to write data size=@ZU, offset_bytes=@ZU fd=@FD buff=@BUF", buff_size, section_start_byte_abs, write_params->fd, buff);
		goto out;
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

/* get the pba_s member of metadata gpt entry of a segment (helper) */
static uint64_t disk_segment_get_seg_metadata_pba_s_from_gpt(struct nvmeibt_disk_segment *disk_segment)
{
	const struct nvmeibt_disk_gpt_partition_entry	*seg_metadata_gpt_entry;
	uint64_t										pba_s = (uint64_t) -1;

	seg_metadata_gpt_entry = nvmeibt_seg_active_get_metadata_gpt_entry(nvmeibt_disk_segment_get_seg_active(disk_segment));
	if (seg_metadata_gpt_entry) {
		pba_s = seg_metadata_gpt_entry->pba_s;
	}
	return pba_s;
}

int nvmeibt_seg_active_fill_metadata_ctrl_header(struct nvmeibt_seg_active_metadata_ctrl *metadata_ctrl)
{
	int											rv = 0;
	struct nvmeibt_disk_segment_metadata_hdr	*header;

	if (!metadata_ctrl) {
		N_Ef(bs8923l, "metadata_ctrl=NULL");
		goto out;
	}
	header = &(metadata_ctrl->header);
	memset(header, 0, sizeof(*header));
	nvmeibt_strlcpy(header->magic_str, DS_METADATA_MAGIC_STR, sizeof(header->magic_str));
	header->encoding_ver = TOMA_ENCODING_VER;
	header->seg_metadata_version = TOMA_METADATA_VERSION_v2_8_0;
	header->mgmt_db_uuid = *nvmeibt_global_get_mgmt_DB_uuid();
out:
	return rv;
}

int nvmeibt_seg_active_metadata_ctrl_init(struct nvmeibt_seg_active	*seg_active)
{
	int											rv;
	struct nvmeibt_seg_active_metadata_ctrl		*metadata_ctrl = seg_active->persistent_metadata;

	NFIN;
	if (!metadata_ctrl) {
		N_Ef(tt75820, "metadata_ctrl=NULL");
		rv = -1;
		goto out;
	}
	rv = 0;

	memset(metadata_ctrl, 0, sizeof(*metadata_ctrl));
	// initialize metadata remaining fields
	metadata_ctrl->disk_segment_uuid = *nvmeibt_seg_active_UUID(seg_active);
	nvmeibt_strlcpy(metadata_ctrl->hostname, nvmeibt_get_my_hostname(),
			sizeof(metadata_ctrl->hostname));
	metadata_ctrl->is_current_shutdown_clean = 0;
	metadata_ctrl->active_praid_version_major = -1;
	metadata_ctrl->active_praid_version_minor = -1;
	metadata_ctrl->locks_table_pbyte_s = 0;
	metadata_ctrl->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK;
	metadata_ctrl->reservation_mode_version = seg_active->highest_reservation_mode_version;

out:
	NFOUT;
	return rv;
}

int nvmeibt_ds_metadata_ctrl_blk_write(struct seg_persistency_write_params *write_params)
{
	uint64_t										seg_metadata_pbyte_s;
	int 											rv = -1;
	struct nvmeibt_seg_active_metadata_ctrl			*md_ctrl = write_params->live_seg_persistent_metadata;
	struct timespec									now;

	NFIN;

	if (!md_ctrl) {
		N_Ef(trace_5B_toma_dsmd, "seg=@UUID_LE ->persistent_matadata=NULL", nvmeibt_seg_active_UUID(write_params->seg_active));
		goto out;
	}

	seg_metadata_pbyte_s = write_params->seg_metadata_pba_s * write_params->pblk_size;
	if (seg_metadata_pbyte_s == 0) {
		nvmeibt_abort(ES_FATAL);
	}

	getnstimeofday_real(&now);
	md_ctrl->save_timespec_tv_sec = now.tv_sec;

	md_ctrl->locks_table_pbyte_s = seg_metadata_pbyte_s + LOCKS_TABLE_RELATIVE_OFFSET_BYTES;
	if (nvmeibt_seg_active_fill_metadata_ctrl_header(md_ctrl) < 0) {
		goto out;
	}
	memset(md_ctrl->__zeroed_filler_till_4K__, 0, sizeof(*md_ctrl) - offsetof(typeof(*md_ctrl), __zeroed_filler_till_4K__));
	md_ctrl->metadata_ctrl_crc32 = crc32_seedless(md_ctrl, offsetof(typeof(*md_ctrl), metadata_ctrl_crc32));

	NDUMP_SEGMENT_METADATA_CTRL_STRUCT(trace_5E_toma_dsmd, md_ctrl, "Writing");
	if (ds_metadata_write_buff(write_params, md_ctrl, sizeof(*md_ctrl),
							   PERSIST_SECTION_PERSIST_CTRL_BLK) < 0) {
		N_Ef(gjut855, "seg=@UUID_LE Failed to write metadata ctrl blk to disk.", nvmeibt_seg_active_UUID(write_params->seg_active));
		goto out;
	}

	while (error_injection_md_write_pause) {
		N_Tf(kiy9654, "seg=@UUID_LE writing metadata to disk PAUSED due to error injection, waiting", nvmeibt_seg_active_UUID(write_params->seg_active));
		sleep(1);
	}

	N_Tf(skoit95, "seg=@UUID_LE wrote metadata to disk", nvmeibt_seg_active_UUID(write_params->seg_active));
	md_ctrl->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID;
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeibt_ds_metadata_set_error_injection_md_write_pause(int value)
{
	N_Tf(trace_nvmeibt_ds_metadata_set_error_injection_md_write_pause, "Setting error injection for MD write to @INT", value);
	error_injection_md_write_pause = value;
}

int nvmeibt_ds_metadata_validate_and_upgrade_persistent_metadata_as_needed(struct nvmeibt_seg_active_metadata_ctrl *persistent_metadata, const union nvmeib_uuid *seg_uuid)
{
	int												rv = 0;
	struct nvmeibt_disk_segment_metadata_hdr		*header = &(persistent_metadata->header);
	static const unsigned char						zeroed_buf_for_comparison[sizeof(*header)];
	uint32_t										calculated_crc32;
	union nvmeib_uuid								mgmt_db_uuid_union;

	NFIN;
	// Since on the upgrade from v1.2 things were reordered, we must rely on the magic_str
	NDUMP_SEGMENT_METADATA_CTRL_STRUCT(tveys3j, persistent_metadata, "Validating");
	calculated_crc32 = crc32_seedless(persistent_metadata, offsetof(typeof(*persistent_metadata), metadata_ctrl_crc32));	// Before making changes
	persistent_metadata->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK;
	if (strncmp(header->magic_str, DS_METADATA_MAGIC_STR, sizeof(DS_METADATA_MAGIC_STR) - 1)) {
		if (!memcmp(header, zeroed_buf_for_comparison, sizeof(*header))) {
			N_Ef(qoz3ck4, "seg=@UUID_8 empty header. For now assuming that it was never written", nvmeib_uuid_first_4_bytes(seg_uuid));
			rv = 1;
			goto out;
		} else {
			N_Ef(qoz35ho, "seg=@UUID_8 bad metadata magic_str='@STR'", nvmeib_uuid_first_4_bytes(seg_uuid), header->magic_str);
			rv = -1;
			goto out;
		}
	}
	if (header->encoding_ver != TOMA_ENCODING_VER) {
		N_IMf(bhduke8, "seg=@UUID_LE TOMA_ENCODING_VER=@UINT expected @UINT", seg_uuid, header->encoding_ver, TOMA_ENCODING_VER);
	}
	//
	mgmt_db_uuid_union = *nvmeibt_global_get_mgmt_DB_uuid();
	if (memcmp(&header->mgmt_db_uuid, &mgmt_db_uuid_union, sizeof(mgmt_db_uuid_union)) != 0) {
		 N_IMf(js8k5l4, "seg=@UUID_LE mgmt_db_uuid=@UUID_LE expected @UUID_LE", seg_uuid, &header->mgmt_db_uuid, &mgmt_db_uuid_union);
	}
	//
	if  ((header->seg_metadata_version >= TOMA_METADATA_VERSION_v2_8_0) && (header->seg_metadata_version != TOMA_METADATA_VERSION_v2_7_0)) {
		if (calculated_crc32 != persistent_metadata->metadata_ctrl_crc32) {
			N_Ef(xpb3yh5, "calculated_crc32=@X persistent_crc32=@X", calculated_crc32, persistent_metadata->metadata_ctrl_crc32);
			rv = -1;
			goto out;
		}
	}
	switch (header->seg_metadata_version) { // Check the segment metadata version
	case TOMA_METADATA_VERSION_v2_8_0:
	case TOMA_METADATA_VERSION_v2_7_0:
		rv = __verify_persistent_md_unused_buf(persistent_metadata);		// All is good, leave the seg_active->persistent_metadata as is
		persistent_metadata->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID;
		N_Tf(q3c93k4, "seg=@UUID_8 valid seg_metadata_version='@X'", nvmeib_uuid_first_4_bytes(seg_uuid), header->seg_metadata_version);
		break;
	case 0:
		persistent_metadata->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_REQ_UPGRADE;
		N_Ef(bif9s70, "seg=@UUID_8 Old seg_metadata_version='@X'", nvmeib_uuid_first_4_bytes(seg_uuid), header->seg_metadata_version);
		rv = -1;
		break;
	default:
		N_Ef(q3c93d9, "seg=@UUID_8 Unknown seg_metadata_version='@X'", nvmeib_uuid_first_4_bytes(seg_uuid), header->seg_metadata_version);
		rv = -1;
		break;
	}
out:
	NFOUT;
	return rv;
}

int nvmeibt_ds_metadata_locks_table_restore(struct nvmeibt_seg_active *seg_active, bool *is_stale_rebuild_required)
{
	struct nvmeibt_local_disk				*local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	struct nvmeibt_disk_segment				*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	struct nvmeibt_praid					*praid = disk_segment->seg_mgmt.its_praid;
	union nvmeib_lock_blkset_entry			*mmapped_locks_tbl = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);
	char									*disk_blk_buf;
	uint64_t								metadata_start_byte_abs;
	uint64_t								n_blksets = num_blksets_in_disk_segment(disk_segment);
	uint64_t								n_ents_4kblks = calc_n_allocated_4kblks_for_locks_ents_by_n_blksets(n_blksets);
	uint64_t								dma_buf_aligned_size = SECTOR_SIZE * n_ents_4kblks;
	uint64_t								n_stale;
	uint64_t								n_dirty;
	uint64_t								pba_s;
	int										pblk_size;
	int										rv = -1;
	uint32_t								calculated_locks_table_crc32;

	NFIN;
	*is_stale_rebuild_required = 0;
	if (!mmapped_locks_tbl || nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Wf(runh737, "Error: seg=@UUID_8, local_disk=@PTR, locks_tbl=@PTR", nvmeibt_seg_UUID_8(disk_segment), local_disk, mmapped_locks_tbl);
		goto out;
	}

	N_Tf(ry75idj4, "is_current_shutdown_clean=@INT version(persistent=@PRAID_VERSION.@PRAID_VERSION cur=@PRAID_VERSION.@PRAID_VERSION)",
		 seg_active->persistent_metadata->is_current_shutdown_clean,
		 seg_active->persistent_metadata->active_praid_version_major,
		 seg_active->persistent_metadata->active_praid_version_minor,
		 praid->praid_follower.applied_praid_lot.topo_ctx.praid_version_major,
		 praid->praid_follower.applied_praid_lot.topo_ctx.praid_version_minor);

	if (!(seg_active->is_last_shutdown_clean)) {
		N_Wf(t_84_toma_dsmd, "Skipping restore: shutdown was not clean?");
		goto out;
	}

	pba_s = disk_segment_get_seg_metadata_pba_s_from_gpt(disk_segment);
	if (pba_s == (uint64_t) -1) {
		N_Wf(gkitj67, "No metadata entry in restore dirty bits of seg:@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		goto out;
	}

	metadata_start_byte_abs = pba_s * local_disk->from_config.pblk_size;

	pblk_size = local_disk->from_config.pblk_size;
	dma_buf_aligned_size = roundup(dma_buf_aligned_size, pblk_size);

	N_Tf(wggyj86, "Restore locks table from persistency (@UINT64_TX entries over @UINT64_TX blks)", n_blksets, n_ents_4kblks);

	// Allocate pages for read/write block from disk.
	disk_blk_buf = NNVMEIBT_BM_ALIGNED_CALLOC(t_87_toma_dsmd, PAGE_SIZE, dma_buf_aligned_size);

	// Future: If RAM format changes use switch(seg_active->persistent_metadata.seg_metadata_version) for backwards compatibility
	if (ds_metadata_read_buff(seg_active,
							  disk_blk_buf,
							  dma_buf_aligned_size,
							  PERSIST_SECTION_LOCKS_TABLE,
							  nvmeibt_local_disk_dev_file_fd(local_disk),
							  metadata_start_byte_abs,
							  seg_active->persistent_metadata->locks_table_pbyte_s) < 0) {
		N_Wf(fki98e5, "Failed read locks table metadata seg=@UUID_8 start_byte=@UINT64_TX n_4kblks=@UINT64_TX",
			nvmeibt_seg_UUID_8(disk_segment), metadata_start_byte_abs, n_ents_4kblks);
		goto free_buf;
	}
	calculated_locks_table_crc32 = crc32_seedless(disk_blk_buf, dma_buf_aligned_size);
	if (seg_active->persistent_metadata->header.seg_metadata_version >= TOMA_METADATA_VERSION_v2_8_0) {
		if (calculated_locks_table_crc32 != seg_active->persistent_metadata->locks_table_crc32) {
			N_Ef(4cxb59k, "calculated_locks_table_crc32=@X persistent_crc32=@X", calculated_locks_table_crc32, seg_active->persistent_metadata->locks_table_crc32);
			goto free_buf;
		}
	}

	nvmeibt_ds_blkset_entries_sanitize_packed(nvmeibt_praid_is_type_EC(disk_segment->seg_mgmt.its_praid), disk_blk_buf, n_blksets, &n_stale, &n_dirty);
	*is_stale_rebuild_required = (n_stale > 0);
	N_Tf(t_88_toma_dsmd, "Restored seg=@UUID_8 dsk_blk_buf==@PTR-@PTR (@ZU), nonzero n_stale=@UINT64_TX n_dirty=@UINT64_TX from disk to memory",
		 nvmeibt_seg_UUID_8(disk_segment), disk_blk_buf, disk_blk_buf + dma_buf_aligned_size, dma_buf_aligned_size,
		 n_stale, n_dirty);

	nvmeibt_ds_blkset_entries_unpack(mmapped_locks_tbl, disk_blk_buf, disk_segment);
	rv = 0;

free_buf:
	NNVMEIBT_BM_FREE(t_8a_toma_dsmd, disk_blk_buf);

out:
	NFOUT;
	return rv;
}

int ds_metadata_locks_table_store(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_local_disk				*local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	struct nvmeibt_disk_segment				*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	union nvmeib_lock_blkset_entry			*mmapped_locks_tbl = nvmeibt_seg_active_get_locks_tbl_ptr(seg_active);
	char									*disk_blk_buf;
	const uint64_t							n_blksets = num_blksets_in_disk_segment(disk_segment);
	const uint64_t							n_ents_4kblks = calc_n_allocated_4kblks_for_locks_ents_by_n_blksets(n_blksets);
	uint64_t								dma_buf_aligned_size = SECTOR_SIZE * n_ents_4kblks;
	struct seg_persistency_write_params		write_params;
	uint64_t								n_stale;
	uint64_t								n_dirty;
	int										pblk_size;
	int										rv = -1;
	struct nvmeibt_seg_active_metadata_ctrl	*persistent_metadata = seg_active->persistent_metadata;

	NFIN;
	if (!mmapped_locks_tbl || nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Wf(t_8g_toma_dsmd, "Error: seg=@UUID_8, local_disk=@PTR, locks_tbl=@PTR", nvmeibt_seg_UUID_8(disk_segment), local_disk, mmapped_locks_tbl);
		goto out;
	}

	N_Tf(t_8i_toma_dsmd, "Store locks table to persistency (@UINT64_TX entries over @UINT64_TX blks)", n_blksets, n_ents_4kblks);

	pblk_size = local_disk->from_config.pblk_size;
	dma_buf_aligned_size = roundup(dma_buf_aligned_size, pblk_size);

	// Allocate pages for write block to disk.
	disk_blk_buf = NNVMEIBT_BM_ALIGNED_CALLOC(t_8j_toma_dsmd, PAGE_SIZE, dma_buf_aligned_size);

	nvmeibt_ds_blkset_entries___pack(mmapped_locks_tbl, disk_blk_buf, disk_segment);	// Todo: Use the 'rv' and write only what was packed
	nvmeibt_ds_blkset_entries_sanitize_packed(nvmeibt_praid_is_type_EC(disk_segment->seg_mgmt.its_praid), disk_blk_buf, n_blksets, &n_stale, &n_dirty);
	N_Tf(t_8k_toma_dsmd, "Stored nonzero n_stale=@N_STALE dirty=@N_DBITS from memory to disk", n_stale, n_dirty);

	write_params.fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	write_params.pblk_size = local_disk->from_config.pblk_size;
	write_params.live_seg_persistent_metadata = persistent_metadata;
	write_params.seg_active = seg_active;
	write_params.num_blksets = n_blksets;
	write_params.seg_metadata_pba_s = disk_segment_get_seg_metadata_pba_s_from_gpt(disk_segment);
	if (write_params.seg_metadata_pba_s == 0) {
		nvmeibt_abort(ES_FATAL);
	}
	if (write_params.seg_metadata_pba_s == (uint64_t) -1) {
		N_Ef(t_8l_toma_dsmd, "Missing seg metadata for seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		goto out;
	}

	persistent_metadata->locks_table_crc32 = crc32_seedless(disk_blk_buf, dma_buf_aligned_size);
	N_Tf(9jsl4mi, "calculated_locks_table_crc32=@X", persistent_metadata->locks_table_crc32);

	// Write the entire dirty bits data to disk at once.
	if (ds_metadata_write_buff(&write_params,
							   disk_blk_buf,
							   dma_buf_aligned_size,
							   PERSIST_SECTION_LOCKS_TABLE) < 0) {
		N_Tf(t_8m_toma_dsmd, "Failed write to locks table metadata seg=@UUID_8 src_buf=@SRC_BUF n_4kblks=@N_4KBLKS",
			nvmeibt_seg_UUID_8(disk_segment), disk_blk_buf, n_ents_4kblks);
		goto free_buf;
	}

	N_Tf(t_8n_toma_dsmd, "Stored locks table entries to persistency for seg=@UUID_8 dsk_blk_buf=@PTR-@PTR (@ZU)",
		nvmeibt_seg_UUID_8(disk_segment), disk_blk_buf, disk_blk_buf + dma_buf_aligned_size, dma_buf_aligned_size);
	rv = 0;

free_buf:
	NNVMEIBT_BM_FREE(t_8o_toma_dsmd, disk_blk_buf);

out:
	NFOUT;
	return rv;
}

void nvmeibt_ds_metadata_store_on_shutdown(struct nvmeibt_seg_active *seg_active)
{
	struct nvmeibt_disk_segment					*disk_segment;
	struct nvmeibt_local_disk					*local_disk;
	struct nvmeibt_seg_active_metadata_ctrl		*live_seg_persistent_metadata;
	struct seg_persistency_write_params			write_params;
	int											locks_table_store_status;
	int											fd;
	struct nvmeibt_disk_segment_topo_ctx		*seg_topo_ctx;
	struct nvmeibt_praid_topo_ctx				*praid_topo_ctx;
	struct timespec								now;

	NFIN;
	if (!seg_active) {
		goto out;
	}
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);
	praid_topo_ctx = nvmeibt_seg_active_get_praid_applied_topo(seg_active);
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	N_Tf(dgyyr74, "id=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
	if (nvmeibt_disk_segment_is_deprecated_in_config(disk_segment)) {
		N_Wf(riiut85, "id=@UUID_8 depr_flag=@DEPR_FLAG", nvmeibt_seg_active_UUID_8(seg_active), disk_segment->from_config.deprecation_flag);
	}

	if (	nvmeibt_seg_lot_is_deleted_in_config(&disk_segment->seg_follower.applied_seg_lot) ||
			!nvmeibt_disk_segment_is_config_OK(disk_segment) ||
			!nvmeibt_seg_active_get_metadata_gpt_entry(seg_active)) {
		N_Wf(dkkit94, "Non functional seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// allocate page for ctrl meatadata block of disk-segment.
	if (seg_active->persistent_metadata == NULL) {
		N_Ef(ssrru85, "Should never happen seg=@UUID_8 has no persistent_metadata", nvmeibt_seg_active_UUID_8(seg_active));
		seg_active->persistent_metadata = NNVMEIBT_BM_ALIGNED_CALLOC(ruut852, PAGE_SIZE, SECTOR_SIZE);
	}

	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		goto out;
	}

	fd = nvmeibt_local_disk_dev_file_fd(local_disk);	//open file with O_DIRECT (non-cached) and	O_SYNC (block until data is written on disk)

	live_seg_persistent_metadata = seg_active->persistent_metadata;
	NDUMP_SEGMENT_METADATA_CTRL_STRUCT(gjji866, live_seg_persistent_metadata, "Read-before-write");

	if (live_seg_persistent_metadata->is_written_on_disk == NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_NOT_ON_DISK) {
		// metadata control data header is bad, so re-initialize its contents)
		if (nvmeibt_seg_active_metadata_ctrl_init(seg_active) < 0) {
			N_Ef(djjfir5, "Failed to init metadata ctrl for seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
			goto out;
		 }
		N_Tf(dkkfi54, "Re-initialized seg=@UUID_8 metadata", nvmeibt_seg_active_UUID_8(seg_active));
	}

	locks_table_store_status = 0; // TODO: uncomment in scale version.
	if (!nvmeibt_praid_is_jbod(disk_segment->seg_mgmt.its_praid)) {
		locks_table_store_status = ds_metadata_locks_table_store(seg_active);
	}

	live_seg_persistent_metadata->active_praid_version_major = seg_topo_ctx->seg_praid_version_major;
	live_seg_persistent_metadata->active_praid_version_minor = seg_topo_ctx->seg_praid_version_minor;
	live_seg_persistent_metadata->is_current_shutdown_clean = !locks_table_store_status;
	live_seg_persistent_metadata->active_praid_version_major = praid_topo_ctx->praid_version_major;
	live_seg_persistent_metadata->active_praid_version_minor = praid_topo_ctx->praid_version_minor;
	getnstimeofday_real(&now);
	live_seg_persistent_metadata->save_timespec_tv_sec = now.tv_sec;

	write_params.fd = fd;
	write_params.pblk_size = local_disk->from_config.pblk_size;
	write_params.live_seg_persistent_metadata = live_seg_persistent_metadata;
	NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(bewjkhw, seg_active, seg_active->highest_reservation_mode_version);
	write_params.seg_active = seg_active;
	write_params.num_blksets = num_blksets_in_disk_segment(disk_segment);
	write_params.seg_metadata_pba_s = disk_segment_get_seg_metadata_pba_s_from_gpt(disk_segment);
	if (write_params.seg_metadata_pba_s == 0) {
		nvmeibt_abort(ES_FATAL);
	}

	if (write_params.seg_metadata_pba_s == (uint64_t) -1) {
		N_Ef(hfuyrte, "Missing seg metadata for seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		goto out;
	}
    // store the ctrl metadata block last, as it contains pointers to previously updated data, that should be made visible atomically.
	if (nvmeibt_ds_metadata_ctrl_blk_write(&write_params) < 0) {
		NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(bwehq8h, seg_active, seg_active->committed_reservation_mode_version);  // A safe assumption
		goto out;
	}
	NVMEIBT_SEG_ACTIVE_SET_COMMITTED_RESERVATION_MODE_VERSION(e220dld, seg_active, seg_active->submitted_reservation_mode_version);
out:
	// YR: to check the store/restore debug code with sigusr1, comment out this line
	nvmeibt_seg_active_del_ptr_to_locks_tbl(seg_active);
	NFOUT;
}

uint64_t nvmeibt_ds_metadata_size_in_disk_pblks(struct nvmeibt_disk_segment *disk_segment)
{
	uint64_t metadata_size_in_4kblks = 0;
	uint64_t metadata_byte_size = 0;
	uint64_t metadata_size_in_pblks = 0;
	const struct nvmeibt_local_disk	*local_disk = nvmeibt_disk_segment_get_local_disk(disk_segment);
	if (!local_disk) {
		N_Ef(error_ds_metadata_nvmeibt_ds_metadata_size_in_disk_pblks, "No local_disk for seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		goto out;
	}

	metadata_size_in_4kblks = divroundup(FIXED_PART_METADATA_SIZE, SECTOR_SIZE) + calc_n_allocated_locks_ents_4kblks(disk_segment);

	metadata_byte_size = metadata_size_in_4kblks * SECTOR_SIZE;
	metadata_size_in_pblks = divroundup(metadata_byte_size, local_disk->from_config.pblk_size);

	N_Tf(trace_ds_metadata_nvmeibt_ds_metadata_size_in_disk_pblks, "seg=@UUID_8 n_4kblks=@N_4KBLKS_LLONG pblk_size=@PBLK_SIZE metadata_size_in_4kblks=@METADATA_SIZE_IN_4KBLKS metadata_size_in_pblks=@METADATA_SIZE_IN_PBLKS n_locks_4kb=@LOCKS_4KB",
		nvmeibt_seg_UUID_8(disk_segment),
		disk_segment->seg_mgmt.lb_e - disk_segment->seg_mgmt.lb_s + 1,
		local_disk->from_config.pblk_size,
		metadata_size_in_4kblks,
		metadata_size_in_pblks,
		calc_n_allocated_locks_ents_4kblks(disk_segment));

out:
	return metadata_size_in_pblks;
}

/**
 * Read segment metadata control block from disk
 * Used by gpt_util for JSON export and diagnostic purposes
 * Returns 0 on success, -1 on error
 */
int nvmeibt_ds_metadata_ctrl_blk_read(struct netlink_io_context *nl_ctx, int fd,
									   int pblk_size, uint64_t pbyte_s,
									   struct nvmeibt_seg_active_metadata_ctrl *seg_md_ctrl)
{
	int				rv = -1;
	uint32_t		read_crc32;
	uint32_t		calculated_crc32;

	NFIN;
	N_Tf(read_seg_md_ctrl, "Reading segment metadata ctrl from pbyte_s=@OFFSET_INT size=@LD", pbyte_s, sizeof(*seg_md_ctrl));

	// Read the 4K control block from disk. Set min_offset_allowed to pblk_size*4 just like disk_metadata_read_disk_metadata.
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, seg_md_ctrl, pbyte_s, pblk_size, sizeof(*seg_md_ctrl), NULL, 0, NVMEIB_IO_IS_READ, 0, pblk_size*4) < 0) {
		N_Ef(read_seg_md_ctrl_io_failed, "Failed to read segment metadata ctrl from pbyte_s=@OFFSET_INT", pbyte_s);
		goto out;
	}

	/* Validate CRC (calculate up to but not including metadata_ctrl_crc32 field) */
	read_crc32 = seg_md_ctrl->metadata_ctrl_crc32;
	calculated_crc32 = crc32_seedless(seg_md_ctrl, offsetof(typeof(*seg_md_ctrl), metadata_ctrl_crc32));

	if (read_crc32 != calculated_crc32) {
		N_Ef(read_seg_md_ctrl_crc_mismatch, "Segment metadata error: pbyte_s=@POS read_crc=@X calculated=@X",
			 pbyte_s, read_crc32, calculated_crc32);
		goto out;
	}
	rv = 0;
	N_Tf(read_seg_md_ctrl_success, "Successfully read segment metadata ctrl: magic=@STR version=@INT uuid=@UUID_LE",
		 seg_md_ctrl->header.magic_str, seg_md_ctrl->header.seg_metadata_version, &seg_md_ctrl->disk_segment_uuid);
out:
	NFOUT;
	return rv;
}

