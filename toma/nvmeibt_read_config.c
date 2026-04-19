/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <linux/limits.h>
#include "nvmeibt_common.h"
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>

#include "nvmeibt_read_config.h"
#include "nvmeibt_node.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_disk.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_local_disk_util.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_kafka.h"
#include "nvmeibt_mm_json.h"

#define DISK_ZEROING_UPDATE_THRESHOLD_SEC 			10

struct nvmeibt_restored_seg_metadata_container {
	struct nvmeibt_seg_active_metadata_ctrl			*metadata_ctrl;		// Includes the header
	int												metadata_gpt_entry_idx;
	struct xdlist									restored_segs_metadata_container_list_link;
	bool											is_valid;
};

struct restore_disk_structures_wq_entry {
	struct nvmeibt_wq_entry 					wq_entry;
	int											fd;
	struct netlink_io_context					*nl_ctx;
	struct nvmeibt_local_disk_config			from_config;
	struct nvmeibt_disk_gpt 					main_gpt;
	struct nvmeibt_disk_gpt 					metadata_gpt;
	struct nvmeibt_disk_format_data 			format_data;
	struct nvmeibt_disk_mbr 					mbr;
	int 										rv;
	BOOL										found_config_on_disk;
	XDLIST_DECLARE(, struct nvmeibt_restored_seg_metadata_container, restored_segs_metadata_container_list_link)  	restored_segs_metadata_container_list;
};

struct local_disk_zero_iter_wq_entry {
	struct nvmeibt_wq_entry 				wq_entry;
	struct local_disk_info					ld_info;
	int 									rv;
	struct nvmeibt_disk_format_data 		format_data;
	int										seq;
	struct nvmeibt_local_disk_util_smart_info 	smart_info;
	struct nvmeibt_ldisk_id_for_srvr_cmd     ldisk;			// Todo: Unite into smart info
	uint64_t								zero_write_counter;
	bool									are_PMBR_and_GPTs_written;
};

#define EXCLUDED_DRIVES_SPEC_FILE_PATH      TOMA_DIR_OPT_NVMESH "/.target_devices"
#define AUTO_TAKEOVER_DRIVES_SPEC_FILE_PATH TOMA_DIR_OPT_NVMESH "/.auto_takeover_drives_spec"

#define MINIMUM_ZERO_THREASHOLD_FOR_INIT 0.01

static const char *nvmeibt_get_csv_section_header_by_section_type(enum NVMEIBT_CSV_TYPE section_type)
{
	#define CSV_BUF_SECTION_HEADER "SECTION NAME: "
	if (     section_type == NVMEIBT_CSV_TYPE_LOCAL_DISKS)	return CSV_BUF_SECTION_HEADER "LOCAL_DISKS";
	else if (section_type == NVMEIBT_CSV_TYPE_LOCAL_NICS)	return CSV_BUF_SECTION_HEADER "LOCAL_NICS";
	else													return NULL;
}

const char *nvmeibt_get_csv_header_by_section_type(enum NVMEIBT_CSV_TYPE section_type)
{
	if (     section_type == NVMEIBT_CSV_TYPE_LOCAL_DISKS)	return NVMEIBS_DISKS_CSV_HEADER;
	else if (section_type == NVMEIBT_CSV_TYPE_LOCAL_NICS)	return NVMEIBS_NICS_CSV_HEADER;
	else													return NULL;
}

size_t nvmeibt_get_csv_hdrlen_by_section_type(enum NVMEIBT_CSV_TYPE section_type)
{
	return ((section_type == NVMEIBT_CSV_TYPE_LOCAL_DISKS) ? sizeof(NVMEIBS_DISKS_CSV_HEADER) : sizeof(NVMEIBS_NICS_CSV_HEADER)) - 1;
}

enum NVMEIBT_CSV_TYPE nvmeibt_get_section_type_by_section_header(const char *hdr)
{
	if (!strncmp(nvmeibt_get_csv_section_header_by_section_type(NVMEIBT_CSV_TYPE_LOCAL_DISKS), hdr, 100))	return NVMEIBT_CSV_TYPE_LOCAL_DISKS;
	if (!strncmp(nvmeibt_get_csv_section_header_by_section_type(NVMEIBT_CSV_TYPE_LOCAL_NICS ), hdr, 100))	return NVMEIBT_CSV_TYPE_LOCAL_NICS;
	else													return NVMEIBT_CSV_TYPE_NONE;
}

void print_config_to_log(struct mm_mgmt_conf *conf, bool is_topo_config)
{
	struct nvmeibt_Str	*conf_str;

	if (conf) {
		conf_str = NNVMEIBT_STR_ALLOC(sgvsxc4);
		mm_print_conf(conf, (nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, conf_str);
		if (is_topo_config) {
			NVMEIBT_LONG_TRACE_WRAPPER(dydi4k0, 0, "TOPO_CONFIG", nvmeibt_Str_str(conf_str), nvmeibt_Str_strlen(conf_str));
		}
		else {
			NVMEIBT_LONG_TRACE_WRAPPER(dydi4k7, 0, "KAFKA_CONFIG", nvmeibt_Str_str(conf_str), nvmeibt_Str_strlen(conf_str));
		}
		NNVMEIBT_STR_FREE(jsdueha, conf_str);
	}
}

bool nvmeibt_read_config_am_i_eligible_to_read_config_directly(void)
{
	// Only the leader reads the config during routine activity
	return (!nvmeibt_raft_is_raft_valid() || nvmeibt_raft_is_leader());
}

int nvmeibt_read_config_file(struct nvmeibt_Str *config_struct, enum NVMEIBT_CSV_TYPE what)
{
	int			rv = -1;
	const char	*str = nvmeibt_get_csv_section_header_by_section_type(what);
	size_t		csv_section_header_line_len;

	NFIN;
	NTOMA_ASSERT(i889i2, str != NULL, "unknown @SECTION_TYPE", what);	// Add my Header line to the buffer
	nvmeibt_Str_sprintf(config_struct, "%s\n", str);
	csv_section_header_line_len = nvmeibt_Str_strlen(config_struct);
	rv = (what == NVMEIBT_CSV_TYPE_LOCAL_DISKS) ?
			nvmeib_srvr_api_lib_get_csv_disks(config_struct) :
			nvmeib_srvr_api_lib_get_csv_nics( config_struct);
	if (rv < 0) {
		N_ETf(t_zzz_25, "Error reading @SECTION_TYPE, @AUTO_ERRNO", what);
	} else {
		const char *csv_header =           nvmeibt_get_csv_header_by_section_type(what);
		const size_t csv_header_line_len = nvmeibt_get_csv_hdrlen_by_section_type(what);
		const char  *csv_header_line_ptr = nvmeibt_Str_str(config_struct) + csv_section_header_line_len;
		rv = 0;
		if (strncmp(csv_header_line_ptr, csv_header, csv_header_line_len)) {
			N_Wf(rtyy765, "@SECTION_TYPE header, len=@LEN_SIZET, expected='@STR', got='@STR'", what, csv_header_line_len, csv_header, csv_header_line_ptr);
			rv = -__LINE__;
		}
	}
	NFOUT;
	return rv;
}

static void connect_seg_to_praid(struct nvmeibt_praid *praid, struct nvmeibt_disk_segment *seg)
{
	seg->seg_mgmt.its_praid = praid;
	seg->seg_leader.baseline_seg_lot.praid_lot = &praid->praid_leader.baseline_praid_lot;
	seg->seg_leader.calculated_seg_lot.praid_lot = &praid->praid_leader.calculated_praid_lot;
	seg->seg_leader.to_report_seg_lot.praid_lot = &praid->praid_leader.to_report_praid_lot;
	seg->seg_follower.committed_seg_lot.praid_lot = &praid->praid_follower.committed_praid_lot;
	seg->seg_follower.applied_seg_lot.praid_lot = &praid->praid_follower.applied_praid_lot;
}

static void check_seg_override(struct nvmeibt_disk_segment *old_seg, struct nvmeibt_disk_segment *seg, int idx)
{
	if (old_seg && !ARE_UUID_EQ(nvmeibt_seg_UUID(old_seg), nvmeibt_seg_UUID(seg)))
		N_Wf(jajau87, "segs override! old_seg=@UUID_8 seg=@UUID_8 idx=@INT", nvmeibt_seg_UUID_8(old_seg), nvmeibt_seg_UUID_8(seg), idx);
}

static void add_seg_to_praid(struct nvmeibt_praid *praid_in, struct nvmeibt_disk_segment *seg)
{
	struct nvmeibt_praid					*praid;
	int8_t									seg_idx;

	praid = seg->seg_mgmt.its_praid;
	if (!praid) {
		praid = praid_in;
		connect_seg_to_praid(praid, seg);
	}
	XDLIST_ADD_TAIL(&praid->praid_mgmt.all_segs_list, seg);
	seg_idx = nvmeibt_disk_segment_idx_in_praid(seg);
	// seg replacement
	N_Tf(hduwrn5, "praid=@UUID_LE idx=@IDX seg=@UUID_8 deprecation_flag=@CHAR", nvmeibt_praid_UUID(praid), seg_idx, nvmeibt_seg_UUID_8(seg), seg->from_config.deprecation_flag);
	switch (seg->from_config.deprecation_flag) {
	case 'S':			// Substitution
		check_seg_override(praid->praid_mgmt.replacement_topo_segs[seg_idx], seg, seg_idx);
		praid->praid_mgmt.replacement_topo_segs[seg_idx] = seg;
		seg->seg_mgmt.is_replacement = 1;
		nvmeibt_disk_segment_mark_is_newly_added_seg_in_all_topos(seg);
		break;
	case 'R':			// Replaced
	default:			// Normal or whatever
		check_seg_override(praid->praid_mgmt.topo_segs[seg_idx], seg, seg_idx);
		praid->praid_mgmt.topo_segs[seg_idx] = seg;
		seg->seg_mgmt.is_replacement = 0;
		break;
	}
	//
	if (praid->praid_mgmt.n_topo_segs < (seg_idx + 1))
		praid->praid_mgmt.n_topo_segs = (seg_idx + 1);
}

void nvmeibt_read_config_add_missing_seg_to_praid(struct nvmeibt_praid *praid, struct nvmeibt_disk_segment *seg)
{

	if ((seg->from_config.deprecation_flag != 'R') && (seg->from_config.deprecation_flag != 'X')) {
		N_Ef(hu7ssa5, "Missing seg=@UUID_8 mark=@CHAR", nvmeibt_seg_UUID_8(seg), seg->from_config.deprecation_flag);
		nvmeibt_abort(ES_FATAL);
	}
	connect_seg_to_praid(praid, seg);

	seg->seg_follower.committed_seg_lot.from_config = seg->from_config;
	N_Tf(hduwr81, "praid=@UUID_LE idx=@IDX missing_seg=@UUID_8", nvmeibt_praid_UUID(praid), nvmeibt_disk_segment_idx_in_praid(seg), nvmeibt_seg_UUID_8(seg));
}

static void add_seg_lot_to_praid_lot(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_praid_lot				*praid_lot;
	int8_t									seg_idx;

	praid_lot = seg_lot->praid_lot;
	XDLIST_ADD_TAIL(&praid_lot->all_seg_lot_list, seg_lot);
	seg_idx = seg_lot->from_config.idx_in_praid;
	N_Tf(iswon7n, "praid=@UUID_LE idx=@IDX seg=@UUID_8 deprecation_flag=@CHAR", nvmeibt_praid_lot_UUID(praid_lot), seg_idx, nvmeibt_seg_lot_UUID_8(seg_lot), seg_lot->from_config.deprecation_flag);
	switch (seg_lot->from_config.deprecation_flag) {
	case 'S':			// Substitution
		praid_lot->replacement_topo_seg_lots[seg_idx] = seg_lot;
		seg_lot->is_replacement = 1;
		break;
	case 'R':			// Replaced
	default:			// Normal or X or whatever
		praid_lot->topo_seg_lots[seg_idx] = seg_lot;
		seg_lot->is_replacement = 0;
		break;
	}
	//
	if (praid_lot->n_topo_seg_lots < (seg_idx + 1))
		praid_lot->n_topo_seg_lots = (seg_idx + 1);
}

#define VALIDATE_ADD_RV(name, add_rv, section_type) \
	if (add_rv == NVMEIBT_ADD_FAILED || add_rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL) { \
		N_Ef(name ## _1, "Add failed, " #section_type " section"); \
		if (add_rv == NVMEIBT_ADD_FAILED) { \
			N_Ef(name ## _2, "Critical error during parsing, aborting"); \
            nvmeibt_abort(ES_FATAL); \
        } \
	}

#define UPDATE_CONFIG_TAG_ACCORDING_TO_TOPO_CONFIG_new(_name, _conf, _config_tag, _obj_hash, _rv, _obj_to_upd_type)        \
({																						\
	_obj_to_upd_type					*_obj;											\
																						\
	_obj = nvmeib_hash_search_uuid(_obj_hash, &_conf->uuid);							\
	if (_obj) {																			\
		if (!NVMEIBT_OBJ_IS_MARKED_OUTDATED(_obj)) {								\
			_obj->config_tag = _config_tag;												\
			_obj->trim_flags &= ~CONFIG_TRIM_TOPO;										\
			_rv = NVMEIBT_ADD_MODIFIED;													\
		} else {																		\
			_rv = NVMEIBT_ADD_SKIPPED;													\
		}																				\
	}																					\
	else {																				\
		_rv = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;										\
	}																					\
})

int nvmeibt_read_config_vol_removed_from_mgmt(struct mm_mgmt_conf *conf, bool is_updating_leader)
{
	int								i, j, k;
	struct nvmeibt_block_device		*blkdev;
	struct nvmeibt_chunk			*chunk;
	struct nvmeibt_praid			*praid;
	struct nvmeibt_disk_segment		*seg;
	int64_t 						old_last_delete_kafka_mgmt_config_offset;

	NFIN;
	// vol is deleted from MGMTonly after TOMA confirmed it. It has no activity
	// Now, mark all of its elements as deleted, and let garbage collection remove them. No need to report it to MGMT anymore
	if (nvmeibt_raft_is_leader() && !is_updating_leader) {
		N_Tf(cbhsjwk, "Leader got MGMT config on its follower, nothing to do");
		goto out;
	}
	old_last_delete_kafka_mgmt_config_offset = nvmeibt_raft_get_my_raft()->last_delete_kafka_mgmt_config_offset;
	for (i = 0; i < conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];
		blkdev = nvmeibt_block_device_get_block_device_by_id(&(vol->uuid));
		if (!blkdev) {
			N_Wf(cvakeo4, "Could not find blkdev=@UUID_LE", &(vol->uuid));
			continue;
		}
		blkdev->is_being_deleted = 1;
		// Cause leader to regenerate to_commit bufs without this blkdev
		SET_RAFT_COMMIT_LIFECYCLE_VAL(f6iso6m, TOPO,        leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,        leader_to_commit) + 1);
		SET_RAFT_COMMIT_LIFECYCLE_VAL(mv0djui, TOPO_CONFIG, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit) + 1);
		nvmeibt_topology_leader_mark_recalc_required();
		nvmeibt_raft_get_my_raft()->last_delete_kafka_mgmt_config_offset = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated);

		// mark MGMT_TRIM for all blkdev objects
		nvmeibt_block_device_trim_specific_block_device(blkdev, CONFIG_TRIM_MGMT);
		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk = blkdev->chunks[j];
			nvmeibt_chunk_trim_specific_chunk(chunk, CONFIG_TRIM_MGMT);
			for (k = 0; k < chunk->n_praids; k++) {
				praid = chunk->praids[k];
				nvmeibt_praid_trim_specific_praid(praid, CONFIG_TRIM_MGMT);
				XDLIST_FOREACH_SAFE(seg, &(praid->praid_mgmt.all_segs_list)) {
					nvmeibt_disk_segment_trim_specific_seg(seg, CONFIG_TRIM_MGMT);
				}
			}
		}
	}
	if (old_last_delete_kafka_mgmt_config_offset != nvmeibt_raft_get_my_raft()->last_delete_kafka_mgmt_config_offset) {
		N_Tf(vol_del_upd_last_delete, "VOL_DEL_COMPLETED: updated last_delete_kafka_mgmt_config_offset=@INT64_TD from @INT64_TD",
			 nvmeibt_raft_get_my_raft()->last_delete_kafka_mgmt_config_offset, old_last_delete_kafka_mgmt_config_offset);
	}
out:
	NFOUT;
	return 0;
}

void nvmeibt_read_config_vol_mark_vol_and_segs_for_removal(struct mm_mgmt_conf *conf, bool is_updating_leader)
{
	int								i, j, k;
	struct nvmeibt_block_device		*blkdev;
	struct nvmeibt_chunk			*chunk;
	struct nvmeibt_praid			*praid;
	struct nvmeibt_disk_segment		*seg;

	NFIN;
	// Mark vol and subtree as X
	// Now, mark all of its elements as deleted, and let garbage collection remove them. No need to report it to MGMT anymore
	if (nvmeibt_raft_is_leader() && !is_updating_leader) {
		N_Tf(ycbskel, "Leader got MGMT config on its follower, nothing to do");
		goto out;
	}
	for (i = 0; i < conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];
		blkdev = nvmeibt_block_device_get_block_device_by_id(&(vol->uuid));
		if (!blkdev) {
			N_Wf(cvsjk9q, "Could not find blkdev=@UUID_LE", &(vol->uuid));
			continue;
		}
		blkdev->from_config.is_deprecated = 1;
		blkdev->serialized_vol_conf.action = 'X';	// Since we got it as a msg and not in a format of a new vol config
		blkdev->from_config.version = VERSION_OF_DELETED_VOL;
		blkdev->serialized_vol_conf.version = VERSION_OF_DELETED_VOL;
		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk = blkdev->chunks[j];
			chunk->from_config.version = VERSION_OF_DELETED_VOL;
			for (k = 0; k < chunk->n_praids; k++) {
				praid = chunk->praids[k];
				praid->from_config.version = VERSION_OF_DELETED_VOL;
				XDLIST_FOREACH_SAFE(seg, &(praid->praid_mgmt.all_segs_list)) {
					seg->from_config.deprecation_flag = 'X';
				}
				NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(ko0ko03, praid);
			}
		}
		nvmeibt_mm_json_mark_deleted_in_kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf(
			blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.data_buf, blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.buf_len);
	}
out:
	NFOUT;
}

int nvmeibt_read_config_apply_vol_mgmt_conf(struct mm_mgmt_conf *conf, int vol_config_tag, bool is_updating_leader, enum KAFKA_EVENT_TYPE event_type, bool is_topo_config)
{
	enum nvmeibt_add_rv				add_rv = NVMEIBT_ADD_UNINITIALIZED;
	enum nvmeibt_add_rv				praid_add_rv;
	int								i, j, k, l;
	struct nvmeibt_block_device		*blkdev;
	int64_t							committed_idx, highest_seen_committed_idx;
	struct nvmeibt_chunk			*chunk;

	NFIN;

	committed_idx = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed);
	highest_seen_committed_idx = nvmeibt_global_get_global()->highest_seen_committed_kafka_mgmt_config_idx;
	if (nvmeibt_raft_is_leader() && !is_updating_leader) {
		if (committed_idx > highest_seen_committed_idx) {
			N_Tf(rys99bq,"committed_idx=@INT64, highest_seen=@INT64", committed_idx, highest_seen_committed_idx);
			nvmeibt_global_get_global()->highest_seen_committed_kafka_mgmt_config_idx = committed_idx;
		}
		N_Tf(huju7zz, "Leader got MGMT config on its follower, nothing to do");
		goto out;
	}
	for (i=0; i<conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];

		if (is_updating_leader && (event_type == KAFKA_EVENT_TYPE_VOL_UPD) && !nvmeibt_block_device_get_block_device_by_id(&(vol->uuid))) {
			N_Wf(3nvsiys, "Update of non existing blkdev. Possibly already erased and msgs reordered. Skipping. blkdev=@STR", vol->name);
			continue;
		}
		add_rv = nvmeibt_block_device_add(vol, vol_config_tag, &blkdev, is_topo_config);
		VALIDATE_ADD_RV(8jskw2m, add_rv, volume);
		if (!vol || ((add_rv != NVMEIBT_ADD_MODIFIED) && (add_rv != NVMEIBT_ADD_NEW))) {
			N_Tf(udohetv, "vol=@STR add_rv=@INT, skipping", vol->name, add_rv);
			continue;   // Next volume. Specifically NVMEIBT_ADD_SKIPPED due to lower vol->version
		}
		for (j = 0; j < vol->num_chunks; j++) {
			struct mm_chunk_conf *chunk_conf = &vol->chunks[j];
			add_rv = nvmeibt_chunk_add(chunk_conf, blkdev, j, vol_config_tag, &chunk);
			VALIDATE_ADD_RV(d4dnmd0, add_rv, chunk);
			if (!chunk) {
				continue;
			}
			for (k = 0; k < chunk_conf->num_praids; k++) {
				struct mm_praid_conf *praid = &chunk_conf->praids[k];
				struct nvmeibt_praid *praid_out;
				int n_rep_seg;

				if (praid->stripeIndex == ILLEGAL_STRIPE_INDEX) {
					N_Tf(uuenn33, "dummy praid=@UUID_LE, skiping", &praid->uuid);
					continue;
				}
				praid_add_rv = nvmeibt_praid_add(praid, chunk, vol, is_updating_leader, &praid_out, vol_config_tag);
				VALIDATE_ADD_RV(4bk32kw, praid_add_rv, praid);
				if (!praid_out) {
					N_Ef(y78uqq2, "praid wasn't added");
					continue;
				}
				for (l = 0; l < praid->num_segments; l++) {
					struct mm_segment_conf *seg = &praid->segments[l];
					struct nvmeibt_disk_segment *seg_out;

					add_rv = nvmeibt_disk_segment_add(seg, praid, vol, is_updating_leader, &seg_out, vol_config_tag);
					VALIDATE_ADD_RV(6bsi3lk, add_rv, seg);
					if (!seg) {
						continue;
					}
					if (seg_out && (praid_add_rv != NVMEIBT_ADD_ALREADY_UP_TO_DATE)) {
						add_seg_to_praid(praid_out, seg_out);
					}
				}
				n_rep_seg = nvmeibt_praid_validate_replacement_segs(praid_out);
				if (!nvmeibt_praid_is_deprecated_in_config(praid_out) &&
					(praid->num_segments != (praid_out->praid_mgmt.n_topo_segs + n_rep_seg))) {
					nvmeibt_praid_mark_conf_corrupted(praid_out);
					N_Ef(uu991ng, "n_seg_cfg=@INT, n_seg_top=@INT, n_seg_rep=@INT", praid->num_segments, praid_out->praid_mgmt.n_topo_segs, n_rep_seg);
				}
			}
		}
		// Now that the volume is complete, it can be serialized
		nvmeibt_mm_json_serialize_vol_and_chunks_and_praids_and_segs_kafka_mgmt_config_to_wire(blkdev, vol);
	}

	if (!nvmeibt_raft_is_leader()) {
		if (committed_idx >= highest_seen_committed_idx) {
			N_Tf(ryy73bq,"committed_idx=@INT64, highest_seen=@INT64", committed_idx, highest_seen_committed_idx);
			nvmeibt_global_get_global()->highest_seen_committed_kafka_mgmt_config_idx = committed_idx;
			nvmeibt_chunk_trim_unused_entries(vol_config_tag, CONFIG_TRIM_MGMT);
			nvmeibt_block_device_trim_unused_entries(vol_config_tag, CONFIG_TRIM_MGMT);
			nvmeibt_disk_segment_trim_unused_entries(vol_config_tag, CONFIG_TRIM_MGMT);
			nvmeibt_praid_trim_unused_entries(vol_config_tag, CONFIG_TRIM_MGMT);
		}
		else {
			N_Wf(ryyd8bq,"committed_idx=@INT64_TD, highest_seen=@INT64_TD. Probably a new leader has old config",
				 committed_idx, highest_seen_committed_idx);
		}
	}

out:
	NFOUT;
	return 0;
}

uint16_t nvmeibt_save_praid_wire_data_to(void* wire_out_p, struct mm_praid_conf *praid_conf) {
	void *wire_out_p_start = wire_out_p;
	wire_out_p += nvmeibt_praid_convert_to_wire_via_aligned_tmp(wire_out_p, praid_conf);
	for (int i = 0; i < praid_conf->num_segments; i++) {
		wire_out_p += nvmeibt_seg_convert_to_wire_via_aligned_tmp(wire_out_p, &praid_conf->segments[i]);
	}
	return (uint16_t)(wire_out_p - wire_out_p_start);
}

int nvmeibt_read_config_apply_vol_committed_topo_conf(struct mm_mgmt_conf *conf, int vol_config_tag)
{
	enum nvmeibt_add_rv				add_rv = NVMEIBT_ADD_UNINITIALIZED;
	int								i, j, k;
	int8_t							l;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();
	int64_t							committed_idx, highest_seen_committed_idx;

	NFIN;

	for (i=0; i<conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];

		UPDATE_CONFIG_TAG_ACCORDING_TO_TOPO_CONFIG_new(hyr7513, vol, vol_config_tag, cur_topo->block_devices_hash_by_uuid, add_rv, struct nvmeibt_block_device);
		VALIDATE_ADD_RV(8jsknhm, add_rv, volume);
		for (j=0; j<vol->num_chunks; j++) {
			struct mm_chunk_conf *chunk = &vol->chunks[j];

			UPDATE_CONFIG_TAG_ACCORDING_TO_TOPO_CONFIG_new(hyr7514, chunk, vol_config_tag, cur_topo->chunks_hash_by_uuid, add_rv, struct nvmeibt_chunk);
			VALIDATE_ADD_RV(d4dimd0, add_rv, chunk);
			for (k=0; k<chunk->num_praids; k++) {
				struct mm_praid_conf *praid = &chunk->praids[k];
				struct nvmeibt_praid *praid_out;

				if (praid->stripeIndex == ILLEGAL_STRIPE_INDEX) {
					N_Tf(uuenn22, "dummy praid=@UUID_LE, skiping", &praid->uuid);
					continue;
				}
				nvmeibt_praid_update_committed_lot_config(praid, vol, &praid_out);
				if (!praid_out)
					continue;
				UPDATE_CONFIG_TAG_ACCORDING_TO_TOPO_CONFIG_new(hyr7515, praid, vol_config_tag, cur_topo->praids_hash_by_uuid, add_rv, struct nvmeibt_praid);
				VALIDATE_ADD_RV(4b232kw, add_rv, praid);

				for (l=0; l<praid->num_segments; l++) {
					struct mm_segment_conf *seg = &praid->segments[l];
					struct nvmeibt_disk_segment *seg_out;

					nvmeibt_seg_update_committed_lot_config(seg, praid, vol, praid_out, &seg_out);
					if (seg_out) {
						UPDATE_CONFIG_TAG_ACCORDING_TO_TOPO_CONFIG_new(y57dnj2, seg, vol_config_tag, cur_topo->disk_segments_hash_by_uuid, add_rv, struct nvmeibt_disk_segment);
						VALIDATE_ADD_RV(9sa93op, add_rv, seg);
						add_seg_lot_to_praid_lot(&seg_out->seg_follower.committed_seg_lot);
					}
				}
			}
		}
	}

	// We reach the end of lifecycle when a object disappear from TOPO_CONFIG. It's dangerous to delete objects before.

	committed_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed);
	highest_seen_committed_idx = nvmeibt_global_get_global()->highest_seen_committed_topo_config_idx;
	if (committed_idx >= highest_seen_committed_idx) {
		N_Tf(ryw83bq,"committed_idx=@INT64, highest_seen=@INT64", committed_idx, highest_seen_committed_idx);
		nvmeibt_global_get_global()->highest_seen_committed_topo_config_idx = committed_idx;
		nvmeibt_chunk_trim_unused_entries(vol_config_tag, CONFIG_TRIM_TOPO);
		nvmeibt_block_device_trim_unused_entries(vol_config_tag, CONFIG_TRIM_TOPO);
		nvmeibt_disk_segment_trim_unused_entries(vol_config_tag, CONFIG_TRIM_TOPO);
		nvmeibt_praid_trim_unused_entries(vol_config_tag, CONFIG_TRIM_TOPO);
	}
	else {
		N_Wf(rma18bq,"committed_idx=@INT64_TD, highest_seen=@INT64_TD. Probably a new leader has old config",
			 committed_idx, highest_seen_committed_idx);
	}

	NFOUT;
	return 0;
}

int nvmeibt_read_config_apply_HW_full_config_mgmt_conf(struct HW_mgmt_conf *conf, int HW_config_tag, bool is_trim_missing_objects)
{
	enum nvmeibt_add_rv		add_rv = NVMEIBT_ADD_UNINITIALIZED;
	int						i, j;

	NFIN;
	for (i=0; i<conf->num_disks; i++) {
		struct mm_disk_conf *disk = &conf->disks[i];
		add_rv = nvmeibt_disk_add(disk, HW_config_tag);
		VALIDATE_ADD_RV(kdi54jw, add_rv, disk);
	}
	for (i=0; i<conf->num_nodes; i++) {
		struct mm_node_conf *node = &conf->nodes[i];
		add_rv = nvmeibt_node_add(node, HW_config_tag);
		VALIDATE_ADD_RV(9he6ehm, add_rv, node);
		for (j=0; j<node->num_nics; j++) {
			struct mm_nic_conf *nic = &node->nics[j];
			add_rv = nvmeibt_nic_add(nic, node, HW_config_tag);
			VALIDATE_ADD_RV(8jrir5h, add_rv, nic);
		}
	}
	//
	nvmeibt_global_validate_and_upd_mgmt_DB_uuid(&conf->dbUUID);
	//
	if (is_trim_missing_objects) {
		nvmeibt_disk_trim_unused_entries(HW_config_tag);
		nvmeibt_nic_trim_unused_entries(HW_config_tag);
		nvmeibt_node_trim_unused_entries(HW_config_tag);
	}
	NFOUT;
	return 0;
}

static int parse_bin_config_buf(const char *wire_buff, int wire_buff_len, int64_t *out_kafka_offset, bool is_topo_config, struct nvmeibt_Str *JSON_output)
{
	int						rv;
	struct mm_mgmt_conf     *conf = NULL;

	NFIN;

	// Parse buff into conf.
	if (!strcmp("CNF", wire_buff)) {			// Binary buffer
		// New-style config in packed format
		N_Tf(shun8xc, "Packed configuration (CNF), len @INT", wire_buff_len);
		conf = mm_wire_buf_to_mm_mgmt_conf(wire_buff, is_topo_config, JSON_output);
	} else {
		N_Ef(tsvghjw, "Unknown buffer content, starts with @STR", wire_buff);
		nvmeibt_abort(ES_FATAL);
	}
	if (!conf) {
		rv = -1;
		goto out;
	}
	*out_kafka_offset = (conf ? conf->idx : 0);
	// TODO: after merging with nvmeibt_Buf, convert callers to use it and skip printing if packed size is too large
	print_config_to_log(conf, is_topo_config);

	nvmeibt_global_validate_and_upd_mgmt_DB_uuid(&conf->dbUUID);

	if (is_topo_config)
		nvmeibt_read_config_apply_vol_committed_topo_conf(conf, nvmeibt_global_get_global()->config_tag);
	else
		nvmeibt_read_config_apply_vol_mgmt_conf(conf, nvmeibt_global_get_global()->config_tag, 0, KAFKA_EVENT_TYPE_VOL_UPD, is_topo_config);

	nvmeibt_topology_leader_mark_recalc_required();
	mm_conf_free_tree(conf);
	rv = 0;
out:
	NFOUT;
	return rv;	// 0 if parsed the binary data OK
}

static bool __is_decodable_encoding_ver(uint32_t encoding_ver)
{
	if (encoding_ver == TOMA_ENCODING_VER)
		return true;
	if (encoding_ver == TOMA_ENCODING_VER_OLDEST_SUPPORTED) {		// v2.8 backward compat
		N_Tf(qnbvd68, "Received topo from older TOMA encoding_ver=@HEX08, current=@HEX08", encoding_ver, TOMA_ENCODING_VER);
		return true;
	} else if ((encoding_ver > TOMA_ENCODING_VER) && (encoding_ver <= TOMA_SW_VER)) {	// Forward compat: newer encoding up to our binary capability
		N_Tf(qnbvd69, "Received topo with higher encoding_ver=@X (current=@X, max_decodable=@X)", encoding_ver, TOMA_ENCODING_VER, TOMA_SW_VER);
		return true;
	}
	N_Ef(qnbvd67, "Unknown encoding version=@X (max decodable=@X)", encoding_ver, TOMA_SW_VER);
	return false;
}

static int parse_bin_topo_buf(const char *wire_data_ptr,
							   int wire_data_len,
							   unsigned long long serialization_version,
							   struct nvmeibt_raft_member *remote_member, struct nvmeibt_Str *JSON_output)
{
	enum nvmeibt_add_rv				add_rv = NVMEIBT_ADD_UNINITIALIZED;
	int								i, j;
	struct nvmeibt_Str 				*print_s;
	int								rv = 0;
	struct nvmeibt_Buf				serialized_topo_buf = {(size_t)0, NULL};
	const struct nvmeibt_Buf		wire_topo_buf = {(size_t)wire_data_len, (void *)wire_data_ptr};

	NFIN;
	//
	if (nvmeibt_topology_is_global_bin_topo(wire_data_ptr)) {
		struct nvmeibt_topology_serialized_topo_header	*header;
		struct nvmeibt_praid_serialized_topo			*praid_topo_ptr;
		struct nvmeibt_serialized_seg_leader_topo		*seg_topo_ptr;

		nvmeibt_topology_convert_wire_topo_buf_to_serialized(&serialized_topo_buf, &wire_topo_buf, JSON_output);
		header = (struct nvmeibt_topology_serialized_topo_header *)(serialized_topo_buf.data_buf);

		// sanity check
		if (!__is_decodable_encoding_ver(header->encoding_ver))
			goto out;
		if (header->topo_len != (unsigned int)wire_data_len) {
			N_Ef(ry78uwq, "Corrupted global topo: actual len=@X header->len=@X", wire_data_len, header->topo_len);
			goto out;
		}

		print_s = NNVMEIBT_STR_ALLOC(fhy128e);
		NNVMEIBT_STR_RESIZE_BUF(gy76e38, print_s, 8192);
		nvmeibt_topology_print((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, &wire_topo_buf, 1);
		NVMEIBT_LONG_TRACE_WRAPPER(twyui67, 0, "RECEIVED TOPOLOGY", nvmeibt_Str_str(print_s), nvmeibt_Str_strlen(print_s));
		NNVMEIBT_STR_FREE(tuneu71, print_s);

		nvmeibt_topology_print_versions(header);

		praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)(header + 1);
		for (i = 0; i < header->praids_num; i++) {
			add_rv = nvmeibt_praid_upd_committed_topo(praid_topo_ptr);
			VALIDATE_ADD_RV(nske45i, add_rv, praid_topo);
			seg_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)(praid_topo_ptr + 1);

			for (j = 0; j < praid_topo_ptr->segs_num; j++) {
				add_rv = nvmeibt_seg_follower_upd_committed_seg_topo(seg_topo_ptr);
				VALIDATE_ADD_RV(vjlscns, add_rv, seg_topo);
				seg_topo_ptr++;
			}

			praid_topo_ptr = (struct nvmeibt_praid_serialized_topo *)seg_topo_ptr;
		}
		goto out;
	}

	if (nvmeibt_topology_is_active_bin_topo(wire_data_ptr)) { // Binary follower topo
		struct nvmeibt_active_topo_header				*header_ptr;
		struct nvmeibt_serialized_seg_active_topo		*seg_topo_ptr;
		unsigned int									calc_len;
		static struct nvmeibt_Buf						tmp_active_serialized_and_wire_topo_buf;	// Starts as wire, converted to serialized in-place. Avoid re-alloc

		// Copy the data, because it may be the applied topo itself (if received from myself)
		NNVMEIBT_BUF_RESIZE(qwpmyt7, &tmp_active_serialized_and_wire_topo_buf, (size_t)wire_data_len);
		memcpy(tmp_active_serialized_and_wire_topo_buf.data_buf, wire_data_ptr, (size_t)wire_data_len);

		// Convert the data
		header_ptr = (struct nvmeibt_active_topo_header *)(tmp_active_serialized_and_wire_topo_buf.data_buf);
		nvmeibt_topology_convert_follower_header_le_be(header_ptr);
		// sanity check
		if (!__is_decodable_encoding_ver(header_ptr->encoding_ver))
			goto out;
		calc_len = header_ptr->segs_num * sizeof(struct nvmeibt_serialized_seg_active_topo) + sizeof(*header_ptr);
		if ((header_ptr->topo_len != calc_len) || ((unsigned int)wire_data_len != calc_len)) {
			N_Ef(yeuj122, "Corrupted applied topo: actual len=@INT inside len=@UINT calc len=@UINT",
				 wire_data_len, header_ptr->topo_len, calc_len);
            goto out;
		}

		seg_topo_ptr = (struct nvmeibt_serialized_seg_active_topo *)(header_ptr + 1);
		for (i = 0; i < header_ptr->segs_num; i++) {
			nvmeibt_disk_segment_convert_active_bin_topo_le_be(seg_topo_ptr);
			// NVMEIBT_DISK_SEGMENT_DUMP_ACTIVE_TOPO(5bsjxos, seg_topo_ptr);
			seg_topo_ptr++;
		}
		// Now tmp_active_serialized_and_wire_topo_buf is already serialized (now wire)
		print_s = NNVMEIBT_STR_ALLOC(uiwwm38);
		NNVMEIBT_STR_RESIZE_BUF(ieus8n2, print_s, 8192);
		nvmeibt_topology_follower_print((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, tmp_active_serialized_and_wire_topo_buf.data_buf);
		NVMEIBT_LONG_TRACE_WRAPPER(uy123nf, 0, "RECEIVED ACTIVE TOPOLOGY", nvmeibt_Str_str(print_s), nvmeibt_Str_strlen(print_s));
		NNVMEIBT_STR_FREE(wvfa5mk, print_s);

		seg_topo_ptr = (struct nvmeibt_serialized_seg_active_topo *)(header_ptr + 1);
		for (i = 0; i < header_ptr->segs_num; i++) {
			add_rv = nvmeibt_disk_segment_leader_upd_from_peer_applied(seg_topo_ptr, serialization_version, remote_member);
			VALIDATE_ADD_RV(njdurke, add_rv, follower_seg_topo);
			seg_topo_ptr++;
		}
		goto out;
	}
	rv = -1;

out:
	NNVMEIBT_BM_FREE(bajhr3m, serialized_topo_buf.data_buf);
	NFOUT;
	return rv;
}

int nvmeibt_parse_buf(const char *csv_or_wire_buf, int csv_or_wire_buf_len, int is_updating_leader, unsigned long long serialization_version,
					  struct nvmeibt_raft_member *remote_member, enum NVMEIBT_CSV_TYPE content_type, struct nvmeibt_Str *JSON_output)
{
TODO(If there were changes, then delete the unused entries, and recalc the relationships between entities)
	char					line[NVMEIBT_MAX_CSV_LINE_LENGTH];

	enum nvmeibt_add_rv		add_rv = NVMEIBT_ADD_UNINITIALIZED;
	int						rv = -1;
	const char				*csv_str_end;
	const char				*scan_line_end;
	const char				*scan_line_ptr;
	int						line_len;
	enum NVMEIBT_CSV_TYPE	section_type = NVMEIBT_CSV_TYPE_NONE;
	int						is_expecting_csv_header_line = 0;
	int						idx_in_section __attribute__((unused)) = -1;
//	int						csv_trunc_len = 0;
	//	struct nvmeibt_topology	*cur_topo = nvmeibt_global_get_global();
	int64_t					parsed_kafka_offset;

	NFIN;

	if (csv_or_wire_buf_len == 0) {
		N_Tf(jsbch3r, "Empty csv. Ignoring.");
		goto out;
	}
	if (csv_or_wire_buf_len == -1) {
		N_Ef(vy8b2b4, "Corrupted topo len");
		rv = -1;
		goto out;
	}
	/*
	 * config_tag is a monotonic counter to mark objects added to the hash
	 * tables, to help identify and prune stale objects after an update:
	 *  - before parsing, incrememt the timestamp
	 *  - during parsing, adjust the timestamp on each object added/updated
	 *  - after pasring, scan the hash and remove objects with old timestamp
	 */
	nvmeibt_global_get_global()->config_tag++;

	// The content (config/topo) is detected from the content of the "csv_buf", but explicit behavior is less error prone
	switch (content_type) {
	case NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES:
		NTOMA_ASSERT(d63k59w, !is_updating_leader, "NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES && is_updating_leader");
		rv = parse_bin_config_buf(csv_or_wire_buf, csv_or_wire_buf_len, &parsed_kafka_offset, 1, JSON_output);
		if (rv == 0) {
			nvmeibt_global_get_global()->is_valid_topo_config_received = 1;
		}
		break;
	case NVMEIBT_CSV_TYPE_FULL_KAFKA_MGMT_CONFIG_VOLUMES:
		rv = parse_bin_config_buf(csv_or_wire_buf, csv_or_wire_buf_len, &parsed_kafka_offset, 0, JSON_output);
		if (rv == 0) {
			SET_RAFT_COMMIT_LIFECYCLE_VAL(hf85kcm, KAFKA_MGMT_CONFIG, follower_applied, parsed_kafka_offset);
		}
		break;
	case NVMEIBT_CSV_TYPE_TOPO:
		rv = parse_bin_topo_buf(csv_or_wire_buf, csv_or_wire_buf_len, serialization_version, remote_member, JSON_output);
		break;
	case NVMEIBT_CSV_TYPE_LOCAL_DISKS:
	case NVMEIBT_CSV_TYPE_LOCAL_NICS:
		N_Tf(jfuwek5, "Received a real CSV");
		break;
	default:
		N_Ef(rvaireo, "Unexpected content_type=@X", content_type);
		goto out;
	}
	if (rv == 0) // A proper parsing already done
		goto out;

	// csv LOCAL_DISKS/LOCAL_NICS
	if (csv_or_wire_buf[csv_or_wire_buf_len - 1] == 0) {
		csv_or_wire_buf_len--;
//		csv_trunc_len = 1;
	}

	csv_str_end = csv_or_wire_buf + csv_or_wire_buf_len;
	scan_line_ptr = csv_or_wire_buf;

	while (scan_line_ptr < csv_str_end) {
		// Get a nice, null_terminated line
		scan_line_end = (char *)memchr(scan_line_ptr, '\n', csv_str_end - scan_line_ptr);
		if (!scan_line_end) {
			N_Tf(tyr6471, "Missing \\n at the end of csv line '@SCAN_LINE_PTR'", scan_line_ptr);
			scan_line_end = csv_str_end;
		}
		line_len = strnlen(scan_line_ptr, scan_line_end - scan_line_ptr);	// Look for '\0' before the '\n'
		if (line_len >= (int)sizeof(line)) {
			NTOMA_ASSERT(nvmeibt_parse_buf_assert, 0, "incorrect usage of csv, long lines? line_len=@INT >= @SIZEOF", line_len, sizeof(line));
			scan_line_ptr = scan_line_end + 1;
			continue;
		}
		memcpy(line, scan_line_ptr, line_len);
		scan_line_ptr = scan_line_ptr + line_len + 1;	// For next line
		line[line_len] = '\0';
		N_Tf(fki98t5, "read line of len=@LEN, '@LINE'", line_len, line);
		if (line_len < 1) {
			continue;
		}
		// _Tf("is_expecting_csv_header_line=%d\n", is_expecting_csv_header_line);
		// Identify new sections start
		if (!memcmp(line, CSV_BUF_SECTION_HEADER, strlen(CSV_BUF_SECTION_HEADER))) {
			// New section
			if (section_type == NVMEIBT_CSV_TYPE_LOCAL_NICS) {
				nvmeibt_local_nic_trim_unused_entries(nvmeibt_global_get_global()->config_tag);
			}
			section_type = nvmeibt_get_section_type_by_section_header(line);
			if (section_type == NVMEIBT_CSV_TYPE_NONE) {
				N_Ef(hy76tr5, "Unknown header type @LINE", line);
				goto out;
			}
			is_expecting_csv_header_line = 1;
			idx_in_section = -1;
		}
		else if (is_expecting_csv_header_line) {
			const char *ref_header = nvmeibt_get_csv_header_by_section_type(section_type);
			// verify and skip the header line
			if (ref_header == NULL) {
				N_Tf(gu8765e, "Wrong @SECTION_TYPE given", section_type);
				goto out;
			}
			if (memcmp(line, ref_header, line_len)) {
				N_Ef(dii9823, "Expecting csv header '@REF_HEADER'. Got '@LINE'", ref_header, line);
				goto out;
			}
			N_Tf(se4566t, "The header csv header line:@REF_HEADER", ref_header);
			is_expecting_csv_header_line = 0;
		}
		else {
			idx_in_section++;
			// _Tf("idx_in_section=%d section_type=%d\n", idx_in_section, section_type);
			// Parse the line
			switch (section_type) {
			case NVMEIBT_CSV_TYPE_LOCAL_DISKS:
				add_rv = nvmeibt_local_disk_add_from_config(line, nvmeibt_global_get_global()->config_tag);
				break;
			case NVMEIBT_CSV_TYPE_LOCAL_NICS:
				add_rv = nvmeibt_local_nic_add(line, nvmeibt_global_get_global()->config_tag);
				break;
			default:
				N_Ef(tu879t3, "Unknown @SECTION_TYPE", section_type);
				add_rv = NVMEIBT_ADD_FAILED;
				break;
			}
			if (add_rv == NVMEIBT_ADD_SKIPPED || add_rv == NVMEIBT_ADD_FAILED || add_rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL) {
				--idx_in_section;
			}
			if (add_rv == NVMEIBT_ADD_FAILED || add_rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL) {
				N_Ef(ttu678w, "Add failed, @SECTION_TYPE, line='@LINE'", section_type, line);
				if (add_rv == NVMEIBT_ADD_FAILED) {
					N_Ef(fyyy732, "\n@STR", csv_or_wire_buf);
					nvmeibt_abort(ES_FATAL);
				}
			}
		}
	}
	if (section_type == NVMEIBT_CSV_TYPE_LOCAL_NICS) {
		nvmeibt_local_nic_trim_unused_entries(nvmeibt_global_get_global()->config_tag);
	}
	if (scan_line_ptr < csv_str_end) {
		N_Tf(uu876f4, "csv_txt actual-length=@LENGTH_LONG != @LENGTH_INT", scan_line_ptr - csv_or_wire_buf, csv_or_wire_buf_len);
		goto out;
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

int nvmeibt_parse_csv_buf(struct nvmeibt_Str *csv_ctx, int is_updating_leader, enum NVMEIBT_CSV_TYPE content_type)
{
	int		rv;
	if (nvmeibt_Str_strlen(csv_ctx) == 0) {
		N_Tf(kio98u7, "Empty csv. Ignoring.");
		rv = 0;
		goto out;
	}
	if (nvmeibt_Str_strlen(csv_ctx) < sizeof(nvmeibt_topology_binary_topo_header)) {
		N_Ef(hv4dsow, "Corrupted ctx len=@STRLEN", nvmeibt_Str_strlen(csv_ctx));
		rv = -1;
		goto out;
	}
	rv = nvmeibt_parse_buf(
					nvmeibt_Str_str(csv_ctx),
					nvmeibt_Str_strlen(csv_ctx),
					is_updating_leader,
					NVMEIBT_NOT_INITIALIZED_SER_VER,
					NULL,
					content_type,
					NULL);
out:
	return rv;
}

/* Notify server about the location of partitions a disk's (journal/serjio-db)*/
static inline int nvmeibr_proc_notify_journal_info(const char* ldisk_id  /*Name of NVMe disk */, uint64_t journal_pba, uint64_t journal_length, uint64_t serjio_db_pba, uint64_t serjio_db_length)
{
	struct nvmeibs_toma_server_proc_buf buf;
	int rv = 0;

	NFIN;
	ZEROINIT(buf);
	buf.type = NVMEIBS_TOMA_JOURNAL_INFO;
	nvmeibt_strlcpy(buf.journal_msg.disk_id, ldisk_id, sizeof(buf.journal_msg.disk_id));
	buf.journal_msg.lba = journal_pba;
	buf.journal_msg.length = journal_length;
	buf.journal_msg.serjio_db_lba = serjio_db_pba;
	buf.journal_msg.serjio_db_length = serjio_db_length;
	if (nvmeib_srvr_api_lib_send_block_msg_to_server(&buf) < 0) {
		N_Ef(t_03_nvmeibt_notify_jour_info, "failed write to local server (@AUTO_ERRNO)");
		rv = 0;	// DHS: Not sure why ??? seems illegal, but at least since 2020
	}
	NFOUT;
	return rv;
}

/* Notify nvmeibs about the existance of the a journal partition and its start address. */
int nvmeibt_read_config_notify_server_about_journal_partition(const struct nvmeibt_disk_gpt_partition_entry *journal_data,
															  const struct nvmeibt_disk_gpt_partition_entry *serjio_db,
															  const char *ldisk_id, const char *ld_display)
{
	int rv = 0;
	if (journal_data && serjio_db) {
		const uint64_t j_len = (journal_data->pba_e - journal_data->pba_s + 1);
		const uint64_t s_len = (serjio_db->pba_e - serjio_db->pba_s + 1);
		N_Tf(op0oer8, "Notifying server about disk=@STR(@STR) journal{pba=@PBA, len=@ZU[blocks]} serjio_db{pba=@PBA, len=@ZU[blocks]}",
			 ld_display, ldisk_id, journal_data->pba_s, j_len, serjio_db->pba_s, s_len);
		rv = nvmeibr_proc_notify_journal_info(ldisk_id, journal_data->pba_s, j_len, serjio_db->pba_s, s_len);
	}
	return rv;
}


int nvmeibt_write_GPTS_of_a_local_disk(struct local_disk_info *ld_info)
{
	int						rv = 0;

TODO(Added local variables just because I couldnt compile during rebase, remove them, and use ld_info directly)
	int									fd = local_disk_info_fd(ld_info);
	struct netlink_io_context 			*nl_ctx = local_disk_info_nl_ctx(ld_info);
	int									pblk_size = local_disk_info_pblk_size(ld_info);
	struct nvmeibt_disk_gpt				*main_gpt = &(ld_info->main_gpt);
	struct nvmeibt_disk_gpt				*metadata_gpt = &(ld_info->metadata_gpt);
	const char							*ldisk_id = ld_info->from_config.ldisk_id.str;
	const char							*ld_display = local_disk_info_display(ld_info);
	bool init_serjio = !(ld_info->is_PMBR_saved_on_disk);

	NFIN;
	// Update changed GPTs on local disks.

	if (!ld_info) {
		rv = -1;
		goto out;
	}
	if (!main_gpt->is_valid) {
		N_Tf(vgak49x, "disk=@STR, GPT is not yet setup, not storing to persistency", ld_display);
		goto out;
	}

	N_Tf(dhbxkxo, "Storing GPTs. disk=@STR gpt_change_no=@GPT_CHANGE_NO", ld_display, ld_info->gpt_change_no);

	//nl_ctx = nvmeibt_make_netlink_context_from_config(&ld_info->from_config);
	if (nl_ctx == NULL) {
		N_Ef(hury349, "NO nl_ctx in ld_info !");
		nvmeibt_abort(ES_FATAL);
	}

	// Rewrite disk GPT.
	if (nvmeibt_disk_metadata_store_gpt(nl_ctx, fd, pblk_size, main_gpt, init_serjio) < 0) {
		N_Ef(bdjhdu, "Unable to store Main-GPT for disk=@STR", ld_display);
		rv = -1;
		goto out;
	}

	// Rewrite metadata GPT on disk. (to update internal mappings of added/removed segments metadata)
	if (nvmeibt_disk_metadata_store_gpt(nl_ctx, fd, pblk_size, metadata_gpt, false) < 0) {
		N_Ef(sbkm49s, "Unable to store metadata-GPT for disk=@STR", ld_display);
		rv = -1;
		goto out;
	}
	// Only after the journal is allocated, and commited to GPT we can notify the server about it.
	rv |= nvmeibt_read_config_notify_server_about_journal_partition(
					nvmeibt_disk_metadata_get_journal_data_entry(main_gpt),
					nvmeibt_disk_metadata_get_serjio_db_entry(main_gpt),
					ldisk_id, ld_display);

out:
	NFOUT;
	return rv;
}

int nvmeibt_write_PMBR_of_a_local_disk(struct netlink_io_context *nl_ctx, int fd, int pblk_size, struct nvmeibt_disk_mbr *mbr, const char *ld_display)
{
	int						rv = -1;
	struct nvmeibt_Str		*dump_str;
	struct nvmeibt_disk_mbr read_mbr;

	NFIN;
	// PMBR is written (if needed) after GPTs to be fully atomic during format, and avoid cases where mbr is written,
	// but GPT is not and we fail a format because of a reboot.
	N_Tf(ndjx9j4, "Disk=@STR writing PMBR", ld_display);

	if (nvmeibt_disk_metadata_write_mbr(nl_ctx, fd, pblk_size, mbr) < 0) {
		N_Ef(7sbhfsk, "Unable to write MBR for disk=@STR", ld_display);
		goto out;
	}
	// In any case - Regardless of whether we wrote the MBR, the MBR should be valid now
	if (nvmeibt_disk_metadata_read_mbr_blk(nl_ctx, fd, pblk_size, &read_mbr, ld_display, mbr) < 0) {
		N_Ef(jsimw4m, "Unable to read MBR for disk=@STR, ignoring disk", ld_display);
		goto out;
	}
	if (!nvmeibt_disk_metadata_is_protective_mbr(&read_mbr)) {
		N_Ef(ybcjmx6, "Invalid PMBR after write of GPT for disk=@STR", ld_display);
		dump_str = NNVMEIBT_STR_ALLOC(xhbsmn8);
		nvmeibt_disk_metadata_fill_dump_mbr_str(dump_str, &read_mbr);
		N_Ef(c3nas8f, "read_mbr=@READ_MBR", nvmeibt_Str_str(dump_str));
		NNVMEIBT_STR_FREE(znx8j2k, dump_str);
		goto out;
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

int nvmeibt_write_all_disks_GPTs(struct nvmeibt_persistency_wq_entry *entry)
{
	int						rv = 0;
	int						write_rv;
	struct local_disk_info	*ld_info = NULL;

	NFIN;
	// Update changed GPTs on local disks.
	XDLIST_FOREACH_SAFE(ld_info, &entry->local_disks_info_hash) {
		// We are in the context of config&topo, possibly need to update the metadata of a seg
		// No need to write the PMBR. It is written only during early zeroing, before any seg's topo life-cycle.
		write_rv = nvmeibt_write_GPTS_of_a_local_disk(ld_info);
		if (write_rv < 0) {
			rv = write_rv;	// One of the disks failed
		} else {
			ld_info->is_gpt_written = 1;
		}
	}

	NFOUT;
	return rv;
}

struct nvmeibt_disk_gpt_partition_entry *add_disk_metadata_partition_to_mem_metadata_gpt(struct nvmeibt_local_disk *cur_local_disk)
{
	struct nvmeibt_disk_gpt_partition_entry		*gpt_entry = NULL;
	union nvmeib_uuid							random_disk_metadata_guid;
	uint64_t									disk_metadata_size_in_pblks = 128ll*1024 / nvmeibt_local_disk_pblk_size(cur_local_disk); // 128KB (minimum alignment unit is is 128KB)

	NFIN;
	generate_random_uuid(&random_disk_metadata_guid);
	// Allocate the journal partition on the disk.
	gpt_entry = nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(&cur_local_disk->metadata_gpt,
																			disk_metadata_size_in_pblks,
																			nvmeibt_local_disk_pblk_size(cur_local_disk),
																			&NVMESH_DISK_METADATA_PARTITION_TYPE_GUID,
																			&random_disk_metadata_guid,
																			DISK_METADATA_PARTITION_NAME,
																			strlen(DISK_METADATA_PARTITION_NAME) + 1,
																			NVMEIBR_PARTITION_ALIGNMENT_4KB);
	if (!gpt_entry) {
		N_Ef(rrtt7y3, "Unable to add metadata-GPT entry for nvmesh disk_metadata on dev:@LOCAL_DISK_FILE_NAME",  nvmeibt_local_disk_file_name(cur_local_disk));
		goto out;
	}
	// Mark this disk GPT changed as we just added a metadata partition.
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(ahsjiu8, cur_local_disk);

out:
	NFOUT;
	return gpt_entry;
}


static int setup_journal_partitions(struct nvmeibt_local_disk *cur_local_disk) {

	int rv = -1;
	union nvmeib_uuid random_journal_guid;
	union nvmeib_uuid random_serjio_guid;
	const unsigned n_bytes_in_blk = cur_local_disk->from_config.pblk_size;
#ifndef TOMA_SIMULATOR_SANDBOX
	const uint64_t journal_data_size_in_pblks = (2ull << 30) / n_bytes_in_blk;		// 2[GB] = Todo, must be == NVMEIB_EC_TOTAL_JOURNAL_BLKS
	const uint64_t serjio_db_size_in_pblks =   (32ull << 20) / n_bytes_in_blk;		// 32[MB], Todo: Take this define from real serjio
#else
	const uint64_t journal_data_size_in_pblks = (1 << 20) / n_bytes_in_blk, serjio_db_size_in_pblks = (1 << 20) / n_bytes_in_blk;		// 1[MB] each. Meaningless
#endif
	NFIN;

	generate_random_uuid(&random_journal_guid);
	generate_random_uuid(&random_serjio_guid);

	if (nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(&cur_local_disk->main_gpt,
																	journal_data_size_in_pblks,
																	n_bytes_in_blk,
																	&NVMESH_JOURNAL_DATA_PARTITION_TYPE_GUID,
																	&random_journal_guid,
																	NVMESH_JOURNAL_PARTITION_NAME,
																	strlen(NVMESH_JOURNAL_PARTITION_NAME) + 1,
																	NVMEIBR_PARTITION_ALIGNMENT_1MB) == NULL)	{
		N_Ef(oo0oir9, "Unable to allocate journal data partition on disk=@STR", nvmeibt_local_disk_display(cur_local_disk));
		goto out;
	}

	// Allocate the serjio_db partition inside the GPT.
	if (nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(&cur_local_disk->main_gpt,
																	serjio_db_size_in_pblks,
																	n_bytes_in_blk,
																	&NVMESH_SERJIO_DB_PARTITION_TYPE_GUID,
																	&random_serjio_guid,
																	NVMESH_SERJIO_DB_PARTITION_NAME,
																	strlen(NVMESH_SERJIO_DB_PARTITION_NAME) + 1,
																	NVMEIBR_PARTITION_ALIGNMENT_1MB) == NULL)	{
		N_Ef(bb889es, "Unable to allocate serjio_db partition on disk=@STR", nvmeibt_local_disk_display(cur_local_disk));
		goto out;
	}

	// Mark this disk GPT changed as we just added a journal partition.
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(sjduf87, cur_local_disk);
	rv = 0;

out:
	NFOUT;
	return rv;
}

static int setup_metadata_gpt(struct nvmeibt_local_disk *cur_local_disk, const union nvmeib_uuid *disk_obj_uuid)
{
	int											rv = 0;
	uint64_t									metadata_partition_size_in_pblks;
	struct nvmeibt_disk_gpt_partition_entry		*metadata_gpt_entry;

	NFIN;
	// The metadata partition is around 0.5% of the disk. The first 0.5% is not used for
	// disk segment allocations, but includes also the GPT itself, and the PMBR.
	{
		// Total blocks of the disk allocated for metadata (0.5% of disk size).
		const uint64_t disk_space_allocated_for_metadata_pblks = (uint64_t)(cur_local_disk->from_config.n_pblk * METADATA_PARTITION_RATIO);
		// Blocks consumed by GPT structures (PMBR + GPT header + partition entries array).
		const uint64_t structures_overhead_pblks = cur_local_disk->main_gpt.header.first_usable_pba + 1;
		// Blocks reserved for 4KB sector alignment (2 x 4KB blocks regardless of PBA size).
		const uint64_t alignment_spare_pblks = 2 * (4096 / nvmeibt_local_disk_pblk_size(cur_local_disk));
		// Total overhead blocks (GPT structures + alignment spare).
		const uint64_t overhead_pblks = structures_overhead_pblks + alignment_spare_pblks;

		// Validate disk is large enough to avoid unsigned integer underflow.
		if (overhead_pblks >= disk_space_allocated_for_metadata_pblks) {
			N_Ef(smgsm01, "Disk too small for metadata partition: disk=@STR n_pblk=@UINT64_TX metadata_disk_space=@UINT64_TX overhead=@UINT64_TX structures_overhead=@UINT64_TX alignment_spare=@UINT64_TX",
				 nvmeibt_local_disk_display(cur_local_disk), cur_local_disk->from_config.n_pblk, disk_space_allocated_for_metadata_pblks, overhead_pblks, structures_overhead_pblks, alignment_spare_pblks);
			rv = -1;
			goto out;
		}
		// This will be the amount of actual data space available in the metadata partition on the disk.
		metadata_partition_size_in_pblks = disk_space_allocated_for_metadata_pblks - overhead_pblks;
	}

	// Allocate the metadata partition on the disk.
	metadata_gpt_entry = nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(&cur_local_disk->main_gpt,
																metadata_partition_size_in_pblks,
																nvmeibt_local_disk_pblk_size(cur_local_disk),
																&NVMESH_METADATA_PARTITION_TYPE_GUID,
																disk_obj_uuid,
																NVMESH_METADATA_PARTITION_NAME,
																strlen(NVMESH_METADATA_PARTITION_NAME) + 1,
																NVMEIBR_PARTITION_ALIGNMENT_1MB);
	if (!metadata_gpt_entry) {
		N_Ef(riikg90, "Unable to allocate metadata partition on disk=@STR", nvmeibt_local_disk_display(cur_local_disk));
		rv = -1;
		goto out;
	}
	// init metadata gpt.
	nvmeibt_disk_metadata_init_mem_gpt(cur_local_disk, metadata_gpt_entry->pba_s,
										   metadata_gpt_entry->pba_e,
										   &cur_local_disk->metadata_gpt,
										   nvmeibt_local_disk_pblk_size(cur_local_disk),
										   LARGE_GPT_MAX_NUM_GPT_ENTRIES,
										   disk_obj_uuid);
	// Mark this disk GPT changed as we just added a metadata partition.
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(trace_read_config_setup_metadata_gpt, cur_local_disk);
out:
	NFOUT;
	return rv;
}

#define NVMEIB_UUID_DISTINGUISHABLE_UNINITIALIZED ((union nvmeib_uuid){.ll = {0xDEADBEAFaaaabbbb, 0xddddeeeeDEADBEAF}})
static void clear_gpt(struct nvmeibt_disk_gpt *gpt)
{
	int idx;

	NFIN;
	// Go over all the entries of the GPT and init them to empty (unused) entries.
	for (idx = 0; idx < gpt->max_n_entries; idx++) {
		struct nvmeibt_disk_gpt_partition_entry *gpt_entry = &(gpt->entries[idx]);
		gpt_entry->partition_type_guid = GPT_UNUSED_ENTRY_TYPE_GUID;
		gpt_entry->partition_guid = NVMEIB_UUID_DISTINGUISHABLE_UNINITIALIZED;
		gpt_entry->pba_s = 0;
		gpt_entry->pba_e = 0;
		gpt_entry->attributes = 0;
	}

	gpt->n_entries_in_use = 0;
	gpt->max_n_entries = 0;

	gpt->header.first_usable_pba = 0;
	gpt->header.revision = 0;
	gpt->header.header_size = 0;
	gpt->header.header_crc32 = 0;
	gpt->header.reserved = 0;
	gpt->header.my_pba = 0;
	gpt->header.alternate_pba = 0;
	gpt->header.first_usable_pba = 0;
	gpt->header.last_usable_pba = 0;
	gpt->header.disk_obj_uuid = nvmeib_uuid_null_val;
	gpt->header.partition_entry_pba = 0 ;
	gpt->header.n_partition_entries = 0;
	gpt->header.size_of_partition_entry = 0;
	gpt->header.partition_entry_array_crc32 = 0;

	NFOUT;
}

void parse_nvmesh_formatted_disk_1st_blk(char *input_mbr_buff, struct nvmeibt_disk_format_data *format_data)
{
	char 		*tok = NULL;
	char 		*delim = ",";
	char		*saveptr = NULL;

	NFIN;

	memset(format_data, 0, sizeof(*format_data));

	tok = strtok_r(input_mbr_buff, delim, &saveptr);
	if (tok != NULL) {
		if (!memcmp(tok, NVMESH_FORMATTED_HDR, strlen(NVMESH_FORMATTED_HDR))) {
			N_Tf(trace_read_config_parse_nvmesh_formatted_disk_1st_blk, "Found " NVMESH_FORMATTED_HDR "");
			format_data->is_nvmesh_formatted_and_awaiting_convert_to_pmbr = 1;
			tok = strtok_r(NULL, delim, &saveptr);
			while (tok != NULL) {
				if (strstr(tok, "block_size=") != NULL) {
					char *val = strstr(tok, "=");
					format_data->pblk_size = (unsigned int)strtoul(val + 1, NULL, 0);
				}
				else if (strstr(tok, "metadata_size=") != NULL) {
					char *val = strstr(tok, "=");
					format_data->metadata_size = (unsigned int)strtoul(val + 1, NULL, 0);
				}
				else if (strstr(tok, "is_md_supported=") != NULL) {
					char *val = strstr(tok, "=");
					format_data->is_md_supported = (BOOL)strtoul(val + 1, NULL, 0);
				}
				else if ((strstr(tok, "disk_obj_uuid=") != NULL) ||
						 (strstr(tok, "disk_guid=") != NULL)) {		// "disk_guid" is for backup compatibility (pre 3.1). I expect it to never happen (upgrade during disk format)
					char *val = strstr(tok, "=");
					if (nvmeibt_urn_uuid_str_to_union_uuid(&format_data->disk_obj_uuid, val + 1) < 0) {
						N_Ef(vsunrw9, "Invalid urn_uuid=@UUID, in disk format!", val + 1);
					}
				}
				else if ((strstr(tok, "ldisk_id=") != NULL)) {
					char *val = strstr(tok, "=");
					nvmeibt_strlcpy(format_data->ldisk_id.str, val + 1, sizeof(format_data->ldisk_id.str));
				}
				else if (strstr(tok, "format_request_counter=") != NULL) {
					char *val = strstr(tok, "=");
					format_data->format_request_counter = (unsigned int)strtoul(val + 1, NULL, 0);
				}

				tok = strtok_r(NULL, delim, &saveptr);
			}
		}
	    else {
			N_Wf(fjjgu87, "Disk format is invalid: tok=\"@TOK\" expected=\"@TOK\"", tok, NVMESH_FORMATTED_HDR);
	    }
    }
	N_Tf(qqw0o98, "Read disk_format: is_nvmesh_formatted_and_awaiting_convert_to_pmbr=@BOOL pblk_size=@UINT metadata_size=@UINT is_md_supported=@BOOL "
		 "ldisk_id=@STR disk_obj_uuid=@STR format_request_counter=@UINT",
		 format_data->is_nvmesh_formatted_and_awaiting_convert_to_pmbr, format_data->pblk_size, format_data->metadata_size, format_data->is_md_supported,
		 format_data->ldisk_id.str, nvmeibt_union_uuid_to_urn_uuid(&(format_data->disk_obj_uuid)).str, format_data->format_request_counter);
	NFOUT;
}

static void local_disk_zero_iter_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_zero_iter_wq_entry			*entry;
	uint64_t										zero_pba_s;
	uint64_t										max_n_pblk_to_zero;
	uint64_t										n_pblk_to_zero;
	uint64_t 										first_sw_pba;
	bool											are_hw_blks = false;
	uint64_t										iteration_ratio;
	const struct nvmeibt_disk_flow_params_t			*p;
	struct local_disk_info							*ld_info;
	const struct nvmeibt_disk_gpt_partition_entry	*disk_metadata_gpt_entry;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_zero_iter_wq_entry, wq_entry);
	ld_info = &(entry->ld_info);

	entry->rv = -1;
	entry->are_PMBR_and_GPTs_written = 0;

	zero_pba_s = min(ld_info->from_config.disk_metadata.last_pba_zeroed + 1, ld_info->from_config.n_hw_pblk - 1);
	p = nvmeibt_disk_flow_params_get(entry->ldisk.Model, true);
	iteration_ratio = (p->is_zeroing_mandatory) ? RATIO_TO_ZERO_PER_ITERATION : RATIO_TO_TRIM_PER_ITERATION;
	max_n_pblk_to_zero = max((ld_info->from_config.n_hw_pblk / iteration_ratio), (uint64_t)(MIN_N_BYTES_TO_ZERO_PER_ITERATION / ld_info->from_config.pblk_size));
	// During the first iteration we don't have last_useable_pba, since there is no GPT, so the best guess is the entire
	// disk size, as long as it's not more than max_n_pblk_to_zero. After the first iteration (or first couple of iterations) we will
	// have a GPT which will define the last_useable_pba.
	n_pblk_to_zero = min(max_n_pblk_to_zero,
						 (ld_info->main_gpt.header.last_usable_pba > 0 ? ld_info->main_gpt.header.last_usable_pba + 1 : ld_info->from_config.n_hw_pblk) - zero_pba_s);
	n_pblk_to_zero = nvmeibt_align_n_blks_to_write_to_blkset(zero_pba_s, n_pblk_to_zero, ld_info->from_config.pblk_size);

	first_sw_pba = (roundup((uint64_t)(ld_info->from_config.n_pblk * ld_info->from_config.pblk_size * METADATA_PARTITION_RATIO), ALIGNED_1MB) / ld_info->from_config.pblk_size);

	// Check if we are in the first 0.5% of the disk, which should be zeroed by HW sectors, and the rest by SW sectors.
	if (zero_pba_s < first_sw_pba) {
		are_hw_blks = true;
		n_pblk_to_zero = first_sw_pba - 1 - zero_pba_s;
		// We need to include the "skipped" blocks in the zeroing count, otherwise the math breaks in the next itteration.
		ld_info->from_config.disk_metadata.last_pba_zeroed = zero_pba_s;
		NTOMA_ASSERT(ueir98f, n_pblk_to_zero > 0, "n_pblk is zero, zero_pba_s=@UINT64_TX last_usable_pba=@UINT64_TX", zero_pba_s, ld_info->main_gpt.header.last_usable_pba);
	}
	else {
		// For everything after 0.5%, we must have aligned addresses
		NTOMA_ASSERT(vkki9f5, is_128KB_aligned(zero_pba_s, ld_info->from_config.pblk_size),
					 "disk=@STR zeroing pba_s=@PBA_S pblk_size=@PBLK_SIZE last_zeroed_pba=@LAST_ZEROED_PBA first_sw_pba=@FIRST_SW_PBA is not 128KB aligned!",
					 local_disk_info_display(ld_info), zero_pba_s, ld_info->from_config.pblk_size, ld_info->from_config.disk_metadata.last_pba_zeroed, first_sw_pba);
	}

	N_Tf(eeur8ft, "disk=@STR zeroing pblks start=@START_INT n_pblks=@N_PBLKS_INT, total blks=@BLKS are_hw_blks=@ARE_HW_BLKS",
		 local_disk_info_display(ld_info), zero_pba_s, n_pblk_to_zero, ld_info->from_config.n_pblk, are_hw_blks);
	if (nvmeibt_zero_disk_pblks(&entry->ldisk, zero_pba_s, n_pblk_to_zero, are_hw_blks) < 0) {  // last_usable_lba
		N_Ef(rr54j33, "Unable to zero disk=@STR", local_disk_info_display(ld_info));
		entry->rv = -1;
		goto out;
	}
	// Zeroing succeeded, update the progress in mem and in the disk_metadata on the disk
	ld_info->from_config.disk_metadata.last_pba_zeroed += n_pblk_to_zero;
	if (ld_info->from_config.disk_metadata.last_pba_zeroed > ld_info->from_config.n_hw_pblk) {
		N_Ef(gfllo04, "last_pba_zeroed=@LAST_PBA_ZEROED n_blocks=@N_BLOCKS", ld_info->from_config.disk_metadata.last_pba_zeroed, ld_info->from_config.n_pblk);
		nvmeibt_abort(ES_FATAL);
	}

	if (nvmeibt_local_disk_util_read_smart_info(ld_info->from_config.seq, &entry->smart_info, ld_info->fd) < 0) {
		TODO(Why read the smart on every zeroing iteration);
		N_Wf(ffigujv, "Error reading smart_info for disk @STR", local_disk_info_display(ld_info));
		goto out;
	}
	// Find where (on disk) to write the disk_metadata
	disk_metadata_gpt_entry = nvmeibt_disk_metadata_get_disk_metadata_entry(&(ld_info->metadata_gpt));
	if (!disk_metadata_gpt_entry) {
		N_Ef(5vs74k3, "No disk_metadata partition on disk=@STR", local_disk_info_display(ld_info));
		entry->rv = -1;
		goto out;
	}
	// Write the disk zeroing progress into the disk metadata in this WQ thread
	// Store disk metadata struct on given fd even before we wrote the GPT and PMBR, so it is there once everything is in place.
	// It is OK to write it also if the zeroing will override it. We will rewrite on the next progress
	if (nvmeibt_disk_metadata_write_disk_metadata_due_to_zeroing_progress(ld_info, disk_metadata_gpt_entry->pba_s, disk_metadata_gpt_entry->pba_e) < 0) {
		N_Ef(tcvhga7, "Error storing disk_metadata with zeroing progress for disk=@STR last_pba_zeroed=@UINT64_TD",
			 local_disk_info_display(ld_info), ld_info->from_config.disk_metadata.last_pba_zeroed);
		entry->rv = -1;
		goto out;
	}
	// If finished zeroeing the beginning of disk in this exact iteration. Can write there
	if (	zero_pba_s											<  ld_info->from_config.n_hw_pblk * MINIMUM_ZERO_THREASHOLD_FOR_INIT &&
			ld_info->from_config.disk_metadata.last_pba_zeroed	>= ld_info->from_config.n_hw_pblk * MINIMUM_ZERO_THREASHOLD_FOR_INIT) {
		if (nvmeibt_local_disk_write_PMBR_and_GPTs(ld_info) == 0) {
			entry->are_PMBR_and_GPTs_written = 1;
		}
	}

	// Store the current write counter of the disk in the message.
	entry->zero_write_counter = entry->smart_info.Host_Write_Commands;
	entry->rv = 0;

out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void local_disk_zero_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_persistency_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct nvmeibt_persistency_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(wi98432, entry);

	NFOUT;
}

BOOL create_disk_mem_mbr_gpts_and_nvmesh_partitions(struct nvmeibt_local_disk *local_disk, struct nvmeibt_disk_format_data *format_data)
{
	union nvmeib_uuid disk_obj_uuid;
	BOOL rv = false;

	NFIN;

	nvmeibt_disk_metadata_init_pmbr(&local_disk->mbr, local_disk->from_config.n_pblk, nvmeibt_local_disk_pblk_size(local_disk));

	// Read the disk uuid from the disk format (it is a MUST for NVMEoF disks, otherwise nice to have.
	if (ARE_UUID_EQ(&format_data->disk_obj_uuid, &nvmeib_uuid_null_val)) {
		generate_random_uuid(&disk_obj_uuid);
		N_Tf(cc886mf, "disk=@STR generated random disk_obj_uuid=@UUID_LE", nvmeibt_local_disk_display(local_disk), &disk_obj_uuid);
	}
	else {
		disk_obj_uuid = format_data->disk_obj_uuid;
	}

	if (strlen(format_data->ldisk_id.str) == 0) {
		N_Ef(rrusj5n, "disk=@STR empty ldisk_id", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	N_Tf(hhjjg07, "Initializing disk=@STR disk_obj_uuid=@UUID_LE", nvmeibt_local_disk_display(local_disk), &disk_obj_uuid);

	// Init main GPT on disk
	nvmeibt_disk_metadata_init_mem_gpt(local_disk, 1,
							   local_disk->from_config.n_hw_pblk - 1,
							   &local_disk->main_gpt,
							   nvmeibt_local_disk_pblk_size(local_disk),
							   MAX_NUM_GPT_ENTRIES,
							   &disk_obj_uuid);

	// Setup new metadata partition + GPT.
	if (setup_metadata_gpt(local_disk, &disk_obj_uuid) < 0) {
		N_Ef(ttiy9gf, "Unable to setup metadata GPT on disk=@STR", nvmeibt_local_disk_display(local_disk));
		goto out;
	}

	// Setup new disk_metadata partition in the **metadata_gpt**.
	if (!add_disk_metadata_partition_to_mem_metadata_gpt(local_disk)) {
		N_Ef(rr56eno, "Unable to setup disk_metadata partition on disk=@STR", nvmeibt_local_disk_display(local_disk));
		goto out;
	}

	// Check if we need to setup a journal partition
	if (format_data->is_md_supported) {
		const struct nvmeibt_disk_gpt_partition_entry *journal = nvmeibt_disk_metadata_get_journal_data_entry(&local_disk->main_gpt);
		const struct nvmeibt_disk_gpt_partition_entry *serjio = nvmeibt_disk_metadata_get_serjio_db_entry(&local_disk->main_gpt);

		if (!journal && !serjio) {
			N_Tf(iiu975f, "disk=@STR initializing journal & serjio partitions", nvmeibt_local_disk_display(local_disk));
			// Setup new journal partition.
			if (setup_journal_partitions(local_disk) < 0) {
				N_Ef(nun6562, "Unable to setup journal GPT on disk=@STR", nvmeibt_local_disk_display(local_disk));
				goto out;
			}
		}
		else {
			if (journal || serjio) {
				N_Ef(ff66ty4, "Partial initialization of jouranl/serjio! journal=@JOURNAL serjio=@SERJIO", journal, serjio);
				goto out;
			}
		}
	}
	else {
		N_Tf(eedd53j, "disk=@STR not formatted for EC. Not adding a journal partition", nvmeibt_local_disk_display(local_disk));
	}

	rv = true;

out:
	NFOUT;
	return rv;
}

static void local_disk_zero_iter_finalize(struct nvmeibt_wq_entry *wq_entry);

static struct local_disk_zero_iter_wq_entry * local_disk_zero_iter_wq_create(struct nvmeibt_local_disk *local_disk, uint last_CHANGE_no)
{
	struct local_disk_zero_iter_wq_entry *dst = NNVMEIBT_BM_CALLOC(trace_00_dup_disk_zerowq_ent, sizeof(*dst));

	dst->wq_entry.type =     "LOCAL_DISK_ZERO_ITER";
	dst->wq_entry.execute =  local_disk_zero_iter_wrapper;
	dst->wq_entry.finalize = local_disk_zero_iter_finalize;
	dst->wq_entry.abort =    nvmeibt_toma_wakeup_wq_abort_func;
	dst->wq_entry.free =     local_disk_zero_freer;
	dst->wq_entry.last_CHANGE_no =  last_CHANGE_no;
	dst->smart_info = local_disk->from_config.smart_info;

	return dst;
}

static void local_disk_zero_iter_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_zero_iter_wq_entry		*entry;
	struct nvmeibt_urn_uuid						disk_obj_urn_uuid;
    struct nvmeibt_local_disk					*local_disk = NULL;
	uint64_t									cur_last_usable_pba;
	BOOL										is_done_zeroing;
	struct timespec								diff_timeout;
	struct local_disk_info						*ld_info;
	static struct nvmeibt_Str					*json_payload = NULL;
	uint64_t									last_pba_zeroed;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_zero_iter_wq_entry, wq_entry);
	ld_info = &(entry->ld_info);
	last_pba_zeroed = ld_info->from_config.disk_metadata.last_pba_zeroed;

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&entry->ldisk.ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);

	/* if wq_entry was canceled, then set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(tyu76d0, "canceled format disk=@STR", local_disk_info_display(ld_info));
		entry->rv = -1;
	}

	/*
	 * entry->rv here is tri-state:
	 * see local_disk_zero_iter_wrapper() for details
	 */

	if (entry->rv < 0) {
		N_ETf(iiy87bg, "Error formatting disk=@STR stopping format", local_disk_info_display(ld_info));
		goto err;
	}

	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Wf(sskiu94, "local_disk=@STR is_being_deleted", local_disk_info_display(ld_info));
		goto err;
	}

	N_Tf(dit886b, "finalize disk=@STR init",  nvmeibt_local_disk_display(local_disk));

	local_disk->is_PMBR_saved_on_disk |= entry->are_PMBR_and_GPTs_written;

	if (local_disk->should_relaunch_format_on_the_next_zeroing_finalize) {
		int format_rv = 0;

		N_Tf(yyuit86, "Aborted format on disk=@STR because abort is triggered",  nvmeibt_local_disk_display(local_disk));
		local_disk->should_relaunch_format_on_the_next_zeroing_finalize = false;
		local_disk->is_being_formatted = true; /*Should be true anywa, just in case*/

		// Now try to start the new format.

		if ((format_rv = nvmeibt_local_disk_launch_disk_format(local_disk)) < 0) {
			if (format_rv == -2)
				goto out;

			N_Ef(eedo0t6, "Error starting new format on disk=@STR block_size=@BLOCK_SIZE metadata_size=@METADATA_SIZE uuid=@UUID_LE",
				 local_disk->pending_format.ld_display, local_disk->pending_format.block_size, local_disk->pending_format.metadata_size, &(local_disk->pending_format.disk_obj_uuid));
			goto err;
		}
	}

	cur_last_usable_pba = (ld_info->main_gpt.header.last_usable_pba ? ld_info->main_gpt.header.last_usable_pba : ld_info->from_config.n_hw_pblk - 1);
	is_done_zeroing = (last_pba_zeroed >= cur_last_usable_pba);
	diff_timeout = timespec_sub(nvmeibt_global_get_cur_event_start_time(), local_disk->last_zeroing_update_time);
	if (is_done_zeroing || (diff_timeout.tv_sec > DISK_ZEROING_UPDATE_THRESHOLD_SEC)) {
		if (local_disk->main_gpt.is_valid) {
			disk_obj_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(local_disk->main_gpt.header.disk_obj_uuid));
		} else {
			disk_obj_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(local_disk->pending_format.disk_obj_uuid));
		}
		if (!json_payload) {
			json_payload = NNVMEIBT_STR_ALLOC(trace_1_topology_nvmeibt_topology_report_dirty_progress_to_mgmt);
		}
		nvmeibt_Str_reuse(json_payload);
		nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT
				"\"payload\":{\"zeroWriteCounter\": %lld, \"nZeroedBlks\": %lld, \"diskUUID\": \"%s\", \"node_id\": \"%s\"}}",
				KAFKA_PRODUCER_MSG_HEADER_VAR("driveZeroingProgress", 1),
				entry->zero_write_counter, last_pba_zeroed + 1, disk_obj_urn_uuid.str,
				nvmeibt_get_my_hostname());
		N_Tf(5bsd3uf, "@STR", nvmeibt_Str_str(json_payload));
		nvmeibt_kafka_outgoing_msgs_queue_add(NULL /*disk_obj_urn_uuid.str*/, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW);
		local_disk->last_zeroing_update_time = nvmeibt_global_get_cur_event_start_time();
	}

	if (!local_disk->are_partitions_setup_in_mem && local_disk->is_PMBR_saved_on_disk) {
		N_Tf(fkgi995, "disk=@STR Zeroed TOMA metadata. Setup partitions and mark them as changed.",  nvmeibt_local_disk_display(local_disk));
		local_disk->are_partitions_setup_in_mem = true;
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(azon3nj);
	}

	// Update the number of blks zeroed.
	if (last_pba_zeroed > local_disk->from_config.disk_metadata.last_pba_zeroed) {
		N_Tf(uuy76gt, "disk=@STR, mark disk_metadata change to store zeroing progress.", nvmeibt_local_disk_display(local_disk));
		local_disk->from_config.disk_metadata.last_pba_zeroed = last_pba_zeroed;
	}
	else {
		// We already passed this place and it may be already used by a seg. This can cause DI !
		N_Ef(eerigk8, "disk=@STR, a second zeroing process is running ! entry->last_pba_zeroed=@LAST_PBA_ZEROED disk->last_pba_zeroed=@LAST_PBA_ZEROED",
			nvmeibt_local_disk_display(local_disk),
			last_pba_zeroed,
			local_disk->from_config.disk_metadata.last_pba_zeroed);
		nvmeibt_abort(ES_FATAL);
	}

	if (last_pba_zeroed > cur_last_usable_pba) {
		N_Ef(oouuk78, "Zeroed too much zeroed_till=@UINT64_TD cur_last_usable_pba=@UINT64_TD", last_pba_zeroed, cur_last_usable_pba);
		nvmeibt_abort(ES_FATAL);
	}
	if (last_pba_zeroed < cur_last_usable_pba) {
		struct local_disk_zero_iter_wq_entry	*local_disk_zero_iter_task;

		N_Tf(ru86540, "disk=@STR going to start next zeroing iteration. last_pba_zeroed=@LAST_PBA_ZEROED",
			 nvmeibt_local_disk_display(local_disk), last_pba_zeroed);
		// Start next zeroing iteration... (we're not done yet..)
		local_disk_zero_iter_task = local_disk_zero_iter_wq_create(local_disk, wq_entry->last_CHANGE_no);	// LKJ: The code below is prone to errors, Can easily forget a field here, extend this function

		local_disk_zero_iter_task->ld_info = *ld_info;
		local_disk_zero_iter_task->ldisk = entry->ldisk;
		local_disk_zero_iter_task->format_data = entry->format_data;

		if (nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(local_disk, &(local_disk_zero_iter_task->wq_entry)) != 0) {
			N_Ef(iu97342, "Unable to add local disk zero task to WQ to continue zeroing of disk=@STR!", nvmeibt_local_disk_display(local_disk));
			NNVMEIBT_BM_FREE(wduty86, local_disk_zero_iter_task);
			goto err;
		}
	}
	else { // Format + zeroing (if needed) is done
		snprintf(local_disk->from_config.status, sizeof(local_disk->from_config.status), "Ok");
		local_disk->is_being_formatted = false;
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(vmnz0b1);
		N_Tf(ttyu863, "Disk=@STR vendor=@INT Done zeroing, status=@STR", nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_vendor_id(local_disk),  local_disk->from_config.status);
	}
	goto out;

err:
	if (local_disk) {
		snprintf(local_disk->from_config.status, sizeof(local_disk->from_config.status), "Error");
		N_Ef(yyu87i3, "Error during disk init for disk=@STR status=@STR", nvmeibt_local_disk_display(local_disk), local_disk->from_config.status);
	}
	else {
		N_Ef(tt043du, "Error during disk init for disk=@STR", entry->ldisk.ldisk_id.str);
	}

out:
	NFOUT;
}

static void validate_and_reduce_gpt_size_if_needed(struct nvmeibt_disk_gpt *gpt)
{
	int		i;

	NFIN;
	if (gpt->max_n_entries > MAX_NUM_GPT_ENTRIES) {
		for (i = MAX_NUM_GPT_ENTRIES; i < MAX_NUM_GPT_ENTRIES_FOR_BACKWARDS_COMPATIBILITY; i++) {
			if (nvmeibt_disk_metadata_is_gpt_entry_in_use(&(gpt->entries[i]))) {
				N_Ef(ji96734, "Found an used GPT entry idx=@IDX. GPT size if @GPT_ENTRIES. Aborting.", i, MAX_NUM_GPT_ENTRIES);
				nvmeibt_abort(ES_FATAL);
			}
		}
		N_Tf(t6y2r0d, "reducing @STR:@STR from @NUMBER_OF_PARTITION_ENTRIES to @GPT_ENTRIES entries", gpt->ldisk_id.str, gpt->main_or_metadata, gpt->max_n_entries, MAX_NUM_GPT_ENTRIES);
		gpt->max_n_entries = MAX_NUM_GPT_ENTRIES;
	}
	NFOUT;
}

static int revive_all_segs_that_were_read_from_the_new_disk(struct restore_disk_structures_wq_entry *entry, struct nvmeibt_local_disk *local_disk)
{
	int 											rv = -1;
	struct nvmeibt_restored_seg_metadata_container	*seg_metadata_container;
	union nvmeib_uuid								*seg_uuid;
	bool											is_anything_revived = 0;
	struct nvmeibt_seg_active						*seg_active;

	NFIN;
	if (nvmeibt_local_disk_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(local_disk)) {
		N_Tf(3nsjs9j, "disk=@STR already revived, skipping", nvmeibt_local_disk_display(local_disk));
		rv = 0;
		goto out;
	}
	nvmeibt_local_disk_mark_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(local_disk);	// The disk is OK. Now seg by seg
	XDLIST_FOREACH_SAFE(seg_metadata_container, &(entry->restored_segs_metadata_container_list)) {
		seg_uuid = &(seg_metadata_container->metadata_ctrl->disk_segment_uuid);
		N_Tf(rgf9klx, "disk=@STR @UUID_8", nvmeibt_local_disk_display(local_disk), nvmeib_uuid_first_4_bytes(seg_uuid));
		if (!(seg_metadata_container->is_valid)) {
			N_Tf(74bd6sl, "Skipping invalid seg=@UUID_8", nvmeib_uuid_first_4_bytes(seg_uuid));
			continue;
		}
		seg_active = nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(local_disk, seg_uuid);
		if (!seg_active) {
			// a new seg
			seg_active = nvmeibt_seg_active_create(seg_uuid, local_disk, &(local_disk->metadata_gpt.entries[seg_metadata_container->metadata_gpt_entry_idx]),
												   seg_metadata_container->metadata_ctrl);
			nvmeibt_seg_active_mark_JGC_rebuild_required(seg_active);	// Since we read it from the local disk, possibly needs JGC
		} else {
			N_Tf(93bnx7s, "seg_active=@UUID_8 already exists (probably from TOPO_CONFIG)", nvmeib_uuid_first_4_bytes(seg_uuid));
			nvmeibt_seg_active_upd_metadata_gpt_entry_and_ctrl(seg_active,
															   &(local_disk->metadata_gpt.entries[seg_metadata_container->metadata_gpt_entry_idx]),
															   seg_metadata_container->metadata_ctrl);
		}
		nvmeibt_register_launch_seg_metadata_ctrl_save(seg_active);
		is_anything_revived = 1;
	}
	if (is_anything_revived || 1) {		TODO(|| 1 because ALIVE_ is affected by the fact that the disk is up/down. Finish the optimization);
		nvmeibt_node_locate_my_node();
		nvmeibt_topology_setup_relationships();
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

static int read_disk_metadata_from_a_newly_discovered_local_disk(struct restore_disk_structures_wq_entry *entry)
{
	int 											rv = -1;
	struct nvmeibt_disk_gpt_partition_entry			*gpt_entry;
	int												i;
	uint64_t										seg_metadata_pbyte_s;
	struct nvmeibt_restored_seg_metadata_container	*seg_metadata_container;
	union nvmeib_uuid								seg_uuid;
	int												validate_rv;
	const int										n_entries = MAX_NUM_GPT_ENTRIES;
	struct nvmeibt_disk_metadata					aligned_disk_metadata __attribute__((aligned(PAGE_SIZE)));
	int												n_unused_entries = 0;

	NFIN;
	// Go over all the segments we have in the metadata GPT and read the configuration that is stored inside them.
	N_Tf(wreqygs, "Looping over #@INT and not @INT", n_entries, entry->metadata_gpt.header.n_partition_entries);
	for (i = 0; i < n_entries; i++) {
		gpt_entry = &(entry->metadata_gpt.entries[i]);
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(gpt_entry)) {
			n_unused_entries++;
			continue;
		} else if (ARE_UUID_EQ(&gpt_entry->partition_type_guid, &NVMESH_DISK_METADATA_PARTITION_TYPE_GUID)) {
			if (nvmeibt_disk_metadata_read_disk_metadata(entry->nl_ctx, entry->fd,
														 entry->from_config.pblk_size,
														 gpt_entry->pba_s * entry->from_config.pblk_size,
														 &aligned_disk_metadata) < 0) {
				N_Ef(h218dos, "Unable to read disk metadata from disk=@STR, partition is corrupt", nvmeibt_local_disk_config_display(&(entry->from_config)));
				goto out;
			}
			entry->from_config.disk_metadata = aligned_disk_metadata;
			nvmeibt_local_disk_recover_missing_ldisk_id_in_upgraded_disk_metadata(&(entry->from_config));
			N_Tf(chiled8, "Successfully read disk_metadata from disk=@STR ec_supported=@EC_SUPPORTED",
				 nvmeibt_local_disk_config_display(&(entry->from_config)), entry->from_config.disk_metadata.is_md_supported);
		} else if (ARE_UUID_EQ(&gpt_entry->partition_type_guid, &NVMESH_SEGMENT_METADATA_PARTITION_TYPE_GUID)) {
			char16_str_to_union_nvmeib_uuid(gpt_entry->partition_name, &seg_uuid);
			seg_metadata_pbyte_s = gpt_entry->pba_s * entry->from_config.pblk_size;
			N_Tf(5h38sjo, "Restoring seg=@UUID_8 metadata from entry#@INT", nvmeib_uuid_first_4_bytes(&seg_uuid), i);

			seg_metadata_container = NNVMEIBT_BM_CALLOC(3hs8vnv, sizeof (*seg_metadata_container));
			seg_metadata_container->metadata_ctrl = NNVMEIBT_BM_ALIGNED_CALLOC(49mws9k, PAGE_SIZE, sizeof(*(seg_metadata_container->metadata_ctrl)));
			seg_metadata_container->is_valid = 0;
			seg_metadata_container->metadata_gpt_entry_idx = i;
			XDLIST_INIT_LINK(&(seg_metadata_container->restored_segs_metadata_container_list_link), NULL);
			XDLIST_ADD_TAIL(&(entry->restored_segs_metadata_container_list), seg_metadata_container);

			// Read the ctrl (that includes the header)
			if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(entry->nl_ctx, entry->fd,
																		  seg_metadata_container->metadata_ctrl,
																		  seg_metadata_pbyte_s,
																		  entry->from_config.pblk_size,
																		  sizeof (*(seg_metadata_container->metadata_ctrl)),
																		  NULL, 0, NVMEIB_IO_IS_READ, 0, entry->from_config.pblk_size*4) < 0) {
				N_Ef(7gwc8sk, "Failed to read datablk seg=@UUID_8 metadata from entry#@INT", nvmeib_uuid_first_4_bytes(&seg_uuid), i);
				goto out;
			}
			validate_rv = nvmeibt_ds_metadata_validate_and_upgrade_persistent_metadata_as_needed(seg_metadata_container->metadata_ctrl, &seg_uuid);
			if (validate_rv < 0) {
				N_Ef(vgw834j, "seg=@UUID_8 entry#@INT invalid", nvmeib_uuid_first_4_bytes(&seg_uuid), i);
				continue;	// Next gpt_entry
			} else if (validate_rv > 0) {
				continue;	// Not expecting to find a valid topo/config/dirty/stale
			}

			seg_metadata_container->is_valid = 1;
		}
		else {
			char	part_name[sizeof(gpt_entry->partition_name) / 2 + 1];
			char16_str_to_str(gpt_entry->partition_name, sizeof(part_name) - 1, part_name);
			N_Ef(jhgmk0p, "Metadata GPT entry#@INT Invalid partition type=@UUID_LE name=@STR",
				 i, &(gpt_entry->partition_type_guid), part_name);
			nvmeibt_abort(ES_FATAL);
		}
	}
	N_Tf(wreqygq, "n_unused_entries=@INT", n_unused_entries);
	rv = 0;
out:
	return rv;
}

static void restore_disk_structures_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct restore_disk_structures_wq_entry			*entry;
	const struct nvmeibt_disk_gpt_partition_entry	*metadata_gpt_entry;
	const struct nvmeibt_disk_gpt_partition_entry	*journal_data_gpt_entry;
	const struct nvmeibt_disk_gpt_partition_entry	*serjio_db_gpt_entry;
	int												rc;

	NFIN;

	entry = container_of(wq_entry, struct restore_disk_structures_wq_entry, wq_entry);

	N_Tf(7cqixrf, "disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));

	/*
	 * entry->rv here is tri-state:
	 *   -1: error occurred
	 *    0: disk format is not valid
	 *    1: disk format is valid
	 */
	entry->rv = -1;
	//entry->nl_ctx = nvmeibt_make_netlink_context_from_config(&entry->from_config);
	// Must read the smart-log + config from hardware at this point in order to correctly report the drive to MGMT.
	switch (entry->from_config.disk_type) {
	case NVMEIBT_NVMESH_DISK_TYPE:
	case NVMEIBT_NVME_DISK_TYPE:
		rc = nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_nvme_driver(&entry->from_config, entry->from_config.dev_file_name, entry->fd);
		break;
	case NVMEIBT_EXTERNAL_DISK_TYPE:
		rc = nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_udev(&entry->from_config, entry->from_config.dev_file_name, entry->fd);
		break;
	case NVMEIBT_VIRTUAL_DISK_TYPE:
		rc = nvmeibt_local_disk_util_fill_devinfo_for_vdisk(&entry->from_config, entry->from_config.dev_file_name, entry->fd);
		break;
	default:
		rc = -1;
	}
	if (!rc) {
		N_Ef(cbks8b4, "Error reading smart_info for disk @STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		entry->rv = -1;
		goto out;
	}
	// We need to do read of smart info here as well (disk is guaranteed to be on nvmesh driver
	// already). For disks that are already completely initialized and zeroed we need to update
	// the smart here, otherwise it will be out of date for a few minutes.
	// Same applies for disks where we will have some error restoring the GPT along the way.
	if (nvmeibt_local_disk_util_read_smart_info(entry->from_config.seq, &entry->from_config.smart_info, entry->fd) < 0) {
		N_Ef(cvsyzqx, "Error reading smart_info for disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		goto out;
	}

	N_Tf(g3x92nw, "Reading MBR for disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
	if (nvmeibt_disk_metadata_read_mbr_blk(entry->nl_ctx, entry->fd, entry->from_config.pblk_size, &entry->mbr, entry->from_config.ldisk_id.str, NULL) < 0) {
		N_Ef(5fhf89d, "Failed to read MBR blk from disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		goto out;
	}
	if (!nvmeibt_disk_metadata_is_mbr_any_mbr(&entry->mbr)) {
		// No valid MBR found - init new GPT on disk with PMBR and all NVMesh metadata's
		char read_mbr_copy[sizeof(struct nvmeibt_disk_mbr)];

		N_Tf(xhd7b5n, "No valid mbr found on disk=@STR checking disk format", nvmeibt_local_disk_config_display(&(entry->from_config)));
		snprintf(entry->from_config.status, sizeof(entry->from_config.status), "Not_Initialized");

		// Treat read MBR as disk_format data, and decide what to do with this disk
		memcpy(read_mbr_copy, (char *)&entry->mbr, sizeof(entry->mbr));
		parse_nvmesh_formatted_disk_1st_blk((char *)read_mbr_copy, &entry->format_data);
		if (entry->format_data.is_nvmesh_formatted_and_awaiting_convert_to_pmbr) {
			// Quit and start disk init from finalize.
			N_Tf(vu3b6w0, "disk=@STR is_nvmesh_formatted_and_awaiting_convert_to_pmbr. quitting restore_disk_structures to start disk initialization", nvmeibt_local_disk_config_display(&(entry->from_config)));
			entry->rv = 1;
			goto out;
		}
	}
	else {
		N_Tf(7v8b35x, "Valid MBR found on disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		if (!nvmeibt_disk_metadata_is_protective_mbr(&(entry->mbr))) {
			N_WTf(mziow0e, "Unable to restore GPT for disk=@STR, no PMBR is present. Skipping disk.", nvmeibt_local_disk_config_display(&(entry->from_config)));
			snprintf(entry->from_config.status, sizeof(entry->from_config.status), "Not_Initialized");
			entry->rv = 0;
			goto out;
		}
		N_Tf(v4c7ys2, "PMBR found on disk=@STR, going to restore GPT and other structures.", nvmeibt_local_disk_config_display(&(entry->from_config)));

		if (nvmeibt_disk_metadata_restore_gpt(entry->nl_ctx, entry->fd,
											  entry->from_config.pblk_size,
											  &entry->main_gpt,
											  1LL,
											  entry->from_config.n_hw_pblk - 1,
											  true) < 0) { // PMBR with no valid gpt
			N_WTf(ctx72ma, "Unable to restore GPT for disk=@STR although protective MBR is present. Skipping disk.", nvmeibt_local_disk_config_display(&(entry->from_config)));
			snprintf(entry->from_config.status, sizeof(entry->from_config.status), "Not_Initialized");
			entry->rv = 0;
			goto out;
		}
		// We need to validate that the main GPT size is good enough, for Linux to handle, otherwise trim it for the next time we save it.
		validate_and_reduce_gpt_size_if_needed(&entry->main_gpt);
		// We have a valid PMBR + valid GPT, maybe use them
		N_Tf(cn8n4bg, "Restored valid GPT on disk=@STR check for NVMesh metadata", nvmeibt_local_disk_config_display(&(entry->from_config)));
		// 1. find metadata gpt entry.
		metadata_gpt_entry = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&entry->main_gpt);
		if (!metadata_gpt_entry) { // No valid metadata found on disk with GPT.
			N_Ef(731vsod, "No GPT metadata entry found on disk=@STR, cannot use disk.", entry->from_config.ldisk_id.str);
			snprintf(entry->from_config.status, sizeof(entry->from_config.status), "Not_Initialized");
			entry->rv = 0;
			goto out;
		}
		// 2. restore metadata gpt.
		if (nvmeibt_disk_metadata_restore_gpt(entry->nl_ctx, entry->fd,
											  entry->from_config.pblk_size,
											  &entry->metadata_gpt,
											  metadata_gpt_entry->pba_s,
											  metadata_gpt_entry->pba_e,
											  true) < 0) {
			N_Ef(curbts8, "Unable to restore metadata GPT, from metadata partition entry for disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
			goto out;
		}
		N_Tf(1m9sf50, "Restored valid metadata GPT on disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		if (read_disk_metadata_from_a_newly_discovered_local_disk(entry) < 0) {
			N_Ef(gnduos4, "Unable to read metadata from disk=@STR with valid GPT", nvmeibt_local_disk_config_display(&(entry->from_config)));
			goto out;
		}
		if (entry->from_config.disk_metadata.is_md_supported) {
			// If we created the GPT table for the EC capable disk, it was only after there was enough space for the journal
			// partitions - so they must be present
			journal_data_gpt_entry = nvmeibt_disk_metadata_get_journal_data_entry(&entry->main_gpt);
			// 4. Check if journal partition exists.
			if (!journal_data_gpt_entry) {
				N_Ef(fbuw8x5, "No journal data partition found on EC_capable disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
				goto out;
			}
			serjio_db_gpt_entry = nvmeibt_disk_metadata_get_serjio_db_entry(&entry->main_gpt);
			if (!serjio_db_gpt_entry) {
				N_Ef(vhsuw92, "No serjio db partition found on EC_capable disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
				goto out;
			}
			if (nvmeibt_read_config_notify_server_about_journal_partition(journal_data_gpt_entry, serjio_db_gpt_entry, (const char *)(&entry->from_config.ldisk_id.str),
																		  nvmeibt_local_disk_config_display(&(entry->from_config))) < 0) {
				N_Ef(2bx7g3t, "Unable to notify server about serjio db partition on disk disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
				goto out;
			}
		} else {
			N_Tf(xhe9sm2, "disk=@STR doesn't support EC", nvmeibt_local_disk_config_display(&(entry->from_config)));
		}
	}

	N_Tf(soc97mq, "Successfully restored disk structures for disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
	//nvmeibt_disk_metadata_print_gpt_header(&entry->main_gpt.header, "Main", "Mem");
	//nvmeibt_disk_metadata_print_all_gpt_entries(entry->main_gpt.entries, entry->main_gpt.header.n_partition_entries, true, "Main", "Mem");

	//nvmeibt_disk_metadata_print_gpt_header(&entry->metadata_gpt.header, "Metadata", "Mem");
	//nvmeibt_disk_metadata_print_all_gpt_entries(entry->metadata_gpt.entries, entry->metadata_gpt.header.n_partition_entries, true, "Metadata", "Mem");
	entry->rv = 0;

out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void restore_disk_structures_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct restore_disk_structures_wq_entry			*entry;
	struct nvmeibt_restored_seg_metadata_container	*seg_container;

	NFIN;

	entry = container_of(wq_entry, struct restore_disk_structures_wq_entry, wq_entry);
	XDLIST_FOREACH_SAFE(seg_container, &(entry->restored_segs_metadata_container_list)) {
		XDLIST_DEL(&(seg_container->restored_segs_metadata_container_list_link));
		NNVMEIBT_BM_FREE(5dhndpk, seg_container->metadata_ctrl);
		NNVMEIBT_BM_FREE(30qnxjs, seg_container);
	}
	NNVMEIBT_BM_FREE(ymne0xs, entry);

	NFOUT;
}

static void restore_disk_structures_finalize(struct nvmeibt_wq_entry *wq_entry) {
	struct nvmeibt_local_disk						*local_disk = NULL;
	struct restore_disk_structures_wq_entry			*entry;
	struct nvmeibt_topology							*cur_topo = nvmeibt_global_get_global();
	struct local_disk_zero_iter_wq_entry			*local_disk_zero_iter_task;

	NFIN;

	entry = container_of(wq_entry, struct restore_disk_structures_wq_entry, wq_entry);

	/* if wq_entry was canceled, then set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(fhu7496, "cacncled format disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		entry->rv = -1;
		goto out;
	}

	N_Tf(ju8ur54, "Trying to finalize restore of disk structures for disk=@STR phys_format_request_counter=@FORMAT_REQUEST_COUNTER",
		 nvmeibt_local_disk_config_display(&(entry->from_config)), entry->format_data.format_request_counter);

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&entry->from_config.ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Wf(sr538v5, "disk=@STR it is not found, probably removed. Cannot complete finalization of disk_structures restore", nvmeibt_local_disk_config_display(&(entry->from_config)));
		goto out;
	}

	/*
	 * entry->rv here is tri-state:
	 * see restore_disk_structures_wrapper() for details
	 */

	// Update smart related values
	nvmeibt_local_disk_mark_periodic_reread_smart_counters_just_finished(local_disk, entry->rv >= 0, 1);

	if (entry->rv < 0) {
		N_Ef(vju8t56, "Unable to restore_disk structures disk=@STR", nvmeibt_local_disk_display(local_disk));
		local_disk->is_done_reading_gpt_existing_or_not = 0;
		goto err;
	}

	local_disk->is_done_reading_gpt_existing_or_not = 1;
	local_disk->is_mbr_a_valid_pmbr = nvmeibt_disk_metadata_is_protective_mbr(&(entry->mbr));
	local_disk->is_PMBR_saved_on_disk = local_disk->is_mbr_a_valid_pmbr;
	// Update the values in the given local disk, based on what was read in async mode.
	local_disk->from_config = entry->from_config;

	if ((entry->rv > 0) && !(entry->format_data.is_nvmesh_formatted_and_awaiting_convert_to_pmbr)) {
		N_Ef(vji8r4d, "OOPS disk=@STR rv=@RV and not is_nvmesh_formatted_and_awaiting_convert_to_pmbr", nvmeibt_local_disk_display(local_disk), entry->rv);
		local_disk->is_mbr_a_valid_pmbr = 0;
		entry->rv = -1;
		goto err;
	}

	if (entry->format_data.is_nvmesh_formatted_and_awaiting_convert_to_pmbr) {
		if (local_disk->active_format_request_counter && (entry->format_data.format_request_counter != local_disk->active_format_request_counter)) {
			N_Tf(xki8rft, "disk=@STR was formatted with an old format_request_counter. Ignoring", nvmeibt_local_disk_config_display(&(entry->from_config)));
			goto out;
		}

		// entry->format_data.disk_uuid was parsed from the disk
		create_disk_mem_mbr_gpts_and_nvmesh_partitions(local_disk, &entry->format_data);
		nvmeibt_disk_metadata_init_disk_metadata_struct(local_disk, &entry->format_data);
		local_disk->from_config.disk_metadata.last_pba_zeroed = 1;
		// Must mark the drive as formatting in TOMA, since it is NOT yet in initializing state, which can be set only AFTER,
		// the GPT is created and the format write counter is updated.
		snprintf(local_disk->from_config.status, sizeof(local_disk->from_config.status), "Formatting");

		N_Tf(cki9ft6, "disk=@STR was just formatted. Starting the first disk_zero_iter", nvmeibt_local_disk_config_display(&(entry->from_config)));
		// Start disk_zero_iter chain, and once the GPT&nvmesh-partitions range is zeroed it will create the GPT and various nvmesh related partitions on it.
		local_disk_zero_iter_task = local_disk_zero_iter_wq_create(local_disk, wq_entry->last_CHANGE_no);	// LKJ: The code below is prone to errors, Can easily forget a field here, extend this function
		nvmeibt_local_disk_fill_ld_info(&(local_disk_zero_iter_task->ld_info), local_disk);
		local_disk_zero_iter_task->ld_info.from_config.disk_metadata.last_pba_zeroed = 0;
		local_disk_zero_iter_task->ldisk = nvmeibt_local_disk_config_to_srvr_cmd_disk(&local_disk->from_config);
//		local_disk_zero_iter_task->seq = entry->from_config.seq;
		local_disk_zero_iter_task->format_data = entry->format_data;

		local_disk->is_being_formatted = true;

		// Prepare to write nvmesh disk's gpt, partitions, ...
		NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(6cfgr3m, local_disk);
		NNVMEIBT_LOCAL_DISK_SET_GPT_SUBMITTED_CHANGE_NO(eitf5ux, local_disk, local_disk->gpt_change_no);

		if (nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(local_disk, &(local_disk_zero_iter_task->wq_entry)) != 0) {
			N_Ef(kki221s, "Unable to add local disk init task to WQ, for disk=@STR!", nvmeibt_local_disk_display(local_disk));
			NNVMEIBT_BM_FREE(dki9r54, local_disk_zero_iter_task);
			entry->rv = -1;
			goto err;
		}

		N_Tf(cki7mbt, "Added disk_zero_iter work for disk=@STR", nvmeibt_local_disk_config_display(&(entry->from_config)));
		nvmeibt_local_disk_mark_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(local_disk);
		goto out;
	} else {
		local_disk->mbr = entry->mbr;
		local_disk->main_gpt = entry->main_gpt;
		local_disk->metadata_gpt = entry->metadata_gpt;
	}

	if (local_disk->from_config.disk_metadata.last_pba_zeroed < local_disk->main_gpt.header.last_usable_pba) {
		N_Tf(ski89v5, "disk=@STR last_pba_zeroed=@LAST_PBA_ZEROED < last_useable_pblk=@LAST_USEABLE_PBLK, resume zeroing",
			nvmeibt_local_disk_display(local_disk), local_disk->from_config.disk_metadata.last_pba_zeroed, local_disk->main_gpt.header.last_usable_pba);

		/*We know that on this path the restore is successful, and we managed to restore GPT/MBR and such, otherwise the read-mbr wouldn't lead here*/
		local_disk->are_partitions_setup_in_mem = true;

		local_disk_zero_iter_task = local_disk_zero_iter_wq_create(local_disk, wq_entry->last_CHANGE_no);	// KKK: The code below is prone to errors, Can easily forget a field here, extend this function
		// Start another disk zero_iter. Once the area of the GPT and various nvmesh related partitions are zeroed, they are created.
		// Fill disk format from the data in the disk_metadata partition.
		nvmeibt_local_disk_fill_ld_info(&(local_disk_zero_iter_task->ld_info), local_disk);
		local_disk_zero_iter_task->format_data.pblk_size =       nvmeibt_local_disk_pblk_size(local_disk);
		local_disk_zero_iter_task->format_data.metadata_size =   local_disk->from_config.metadata_n_bytes;
		local_disk_zero_iter_task->format_data.is_md_supported = local_disk->from_config.disk_metadata.is_md_supported;
		local_disk_zero_iter_task->format_data.disk_obj_uuid =   local_disk->main_gpt.header.disk_obj_uuid;
		local_disk_zero_iter_task->format_data.mgmt_db_uuid =	 *nvmeibt_global_get_mgmt_DB_uuid();
		N_Tf(bg73k30, "Setting mgmt_db_uuid=@UUID_LE for disk=@STR", &(local_disk_zero_iter_task->format_data.mgmt_db_uuid), nvmeibt_local_disk_display(local_disk));
		// Launch a task to resume disk zeroing from where we stopped.
		local_disk_zero_iter_task->ldisk = nvmeibt_local_disk_config_to_srvr_cmd_disk(&local_disk->from_config);
//		local_disk_zero_iter_task->seq = entry->from_config.seq;

		if (nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(local_disk, & (local_disk_zero_iter_task->wq_entry)) != 0) {
			N_Ef(aki89d3, "Unable to add local disk init task to WQ, for disk=@STR", nvmeibt_local_disk_display(local_disk));
			NNVMEIBT_BM_FREE(dkuirn2, local_disk_zero_iter_task);
			entry->rv = -1;
			goto err;
		}
		snprintf(local_disk->from_config.status, sizeof(local_disk->from_config.status), "Initializing");
	}

	//nvmeibt_topology_setup_relationships();
	//local_disk->dev_nl_ctx = nvmeibt_make_netlink_context_from_config(&local_disk->from_config);
	revive_all_segs_that_were_read_from_the_new_disk(entry, local_disk);
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(38djasi);
	goto out;

err:
	if (local_disk) {
		char header[MGMT_LOG_MSG_HEADER_LEN];
		char msg[MGMT_LOG_MSG_MSG_LEN];
		// We failed reading persisted config from this disk, we ignore it and effectively evict it. Since it is
		// either unuseable due to a hardware problem or might already have users' data on it.
		snprintf(header, sizeof(header), "disk %.36s(%.24s.%d) is unusable", LOCAL_DISK_LOG_ARGS(local_disk));
		snprintf(msg, sizeof(msg), "Unable to restore persistency from disk (restore_disk_structures stage), dev=%.32s on node=%.32s",
				 nvmeibt_local_disk_file_name(local_disk), nvmeibt_node_name(cur_topo->my_node));
		nvmeibt_kafka_generic_log_msg_to_mgmt_send(NULL, header, msg, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
	}
out:
	NFOUT;
}

int launch_read_of_local_disk_gpt_and_segs_metadata_and_persist(struct nvmeibt_local_disk *local_disk)
{
	int		rv = 0;
	struct restore_disk_structures_wq_entry	*restore_disk_structures_task;

	NFIN;

	// Clear contents of GPT, to avoid leftovers.
	clear_gpt(&local_disk->main_gpt);
	clear_gpt(&local_disk->metadata_gpt);

	local_disk->are_partitions_setup_in_mem = false;

	nvmeibt_strlcpy(local_disk->main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(local_disk->main_gpt.main_or_metadata));
	nvmeibt_strlcpy(local_disk->metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(local_disk->metadata_gpt.main_or_metadata));

	local_disk->main_gpt.ldisk_id = *nvmeibt_local_disk_UUID(local_disk);
	local_disk->metadata_gpt.ldisk_id = *nvmeibt_local_disk_UUID(local_disk);

	// Start a new restore task for the given disk, it can lead to many paths down the road.
	restore_disk_structures_task = NNVMEIBT_BM_CALLOC(38v39s0, sizeof(*restore_disk_structures_task));
	XDLIST_HEAD_INIT(&(restore_disk_structures_task->restored_segs_metadata_container_list));

	restore_disk_structures_task->wq_entry.type = "RESTORE_DISK_STRUCTURES";
	restore_disk_structures_task->wq_entry.execute = restore_disk_structures_wrapper;
	restore_disk_structures_task->wq_entry.finalize = restore_disk_structures_finalize;
	restore_disk_structures_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	restore_disk_structures_task->wq_entry.free = restore_disk_structures_freer;
	restore_disk_structures_task->wq_entry.last_CHANGE_no = 0;
	restore_disk_structures_task->from_config = local_disk->from_config;
	restore_disk_structures_task->fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	restore_disk_structures_task->nl_ctx = nvmeibt_local_disk_dev_nl_ctx(local_disk);
	restore_disk_structures_task->main_gpt = local_disk->main_gpt;
	restore_disk_structures_task->metadata_gpt = local_disk->metadata_gpt;

	if (nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(local_disk, &(restore_disk_structures_task->wq_entry)) != 0) {
		N_Ef(nf43njs, "Unable to add local disk restore_structures to WQ, for disk=@STR!", nvmeibt_local_disk_display(local_disk));
		NNVMEIBT_BM_FREE(l098n6l, restore_disk_structures_task);
		rv = -1;
		goto out;
	}

	local_disk->is_periodic_reread_smart_counters_in_the_air = 1;

out:
	NFOUT;
	return rv;
}

static int read_target_drives_spec_file(target_drives_spec_t *drives_spec_list, char *input_file_path, bool is_mandatory)
{
	int		fd = -1;
	char	str[4096];
	int		rv = 0;
	int 	n_bytes_read = 0;
	char	line[NVMEIBT_MAX_CSV_LINE_LENGTH];
	char	*str_end;
	char	*scan_line_end;
	char	*scan_line_ptr;
	int		line_len;
	int		r;
	struct target_drive	*drive_spec_line;

	fd = NNVMEIBT_OPEN_READ(vjalqow, input_file_path, is_mandatory);
	if (fd < 0) {
		if (errno == ENODEV || errno == ENOENT) {
			N_Tf(wioanpz, "No such file @PATH @AUTO_ERRNO", input_file_path);
			rv = 1;
		} else {
			N_Ef(sko9t67, "Error opening file=@PATH @AUTO_ERRNO", input_file_path);
			rv = -1;
		}
		goto out;
	}
	N_Tf(qew4453, "Opened file @PATH for read.", input_file_path);

	memset(str, 0, sizeof(str));
	if ((n_bytes_read = NNVMEIBT_PREAD(gji86tr, fd, str, sizeof(str), 0, false)) < 0) {
		N_Ef(qwi79vd, "Error while reading the file=@FILE, @AUTO_ERRNO", input_file_path);
		rv = -1;
		goto out;
	}
	// Empty prev content
	XDLIST_FOREACH_SAFE(drive_spec_line, drives_spec_list) {
		XDLIST_DEL(&(drive_spec_line->target_drives_link));
		NNVMEIBT_TOMA_FREE(bgjrhzx1, drive_spec_line);
	}
	//
	str_end = str + n_bytes_read;
	scan_line_ptr = str;

	while (scan_line_ptr < str_end - 1) {    // While one char before the terminating null
										// Get a nice, null_terminated line
		char	nvme_magic[40];

		scan_line_end = (char *)memchr(scan_line_ptr, '\n', str_end - scan_line_ptr);
		if (!scan_line_end) {
			N_Tf(cmkhitr, "Missing \\n at the end of line '@SCAN_LINE_PTR'", scan_line_ptr);
			scan_line_end = str_end;
		}
		line_len = strnlen(scan_line_ptr, scan_line_end - scan_line_ptr);   // Look for '\0' before the '\n'
		nvmeibt_strlcpy(line, scan_line_ptr, line_len + 1);
		N_Tf(7hwjk29, "line=@STR", line);
		scan_line_ptr = scan_line_ptr + line_len + 1;   // For next line

		if (line[0] == '#') {
			continue;
		}

		// Parse all lines and add respected drives to drives_spec_list.
		drive_spec_line = NNVMEIBT_TOMA_CALLOC(bgjrhzx, 1, sizeof(*drive_spec_line));

		/* Drive spec line format:
		   nvme,<serial>,<vendor-id>,<model-number>,<namespace-id>
		*/

		r = nvmeibt_sscanf_csv_line(line,
									's', sizeof(nvme_magic), nvme_magic,
									's', sizeof(drive_spec_line->native_serial.str), drive_spec_line->native_serial.str,
									'u', &drive_spec_line->vendor,
									's', sizeof(drive_spec_line->model_str), drive_spec_line->model_str,
									'u', &drive_spec_line->nsid,
									'\0');
		if (r < 0) {
			goto out;
		}
		XDLIST_ADD_TAIL(drives_spec_list, drive_spec_line);
		N_Tf(ty57493, "disk=@STR vendor=@VENDOR model=@MODEL nsid=@NSID added to drives_spec_list", drive_spec_line->native_serial.str, drive_spec_line->vendor, drive_spec_line->model_str, drive_spec_line->nsid);
	}
out:
	NNVMEIBT_CLOSE(glo09b7, fd);

	return rv;
}

int nvmeibt_read_excluded_target_drives(void)
{
	int		rv;
	NFIN;
	rv = read_target_drives_spec_file(&(nvmeibt_global_get_global()->excluded_drives_spec), EXCLUDED_DRIVES_SPEC_FILE_PATH, 1);
	NFOUT;
	return rv;
}

int nvmeibt_read_auto_takeover_target_drives(void)
{
	int		rv;
	struct nvmeibt_local_disk	*stock_local_disk;

	NFIN;
	rv = read_target_drives_spec_file(&(nvmeibt_global_get_global()->auto_takeover_drives_spec), AUTO_TAKEOVER_DRIVES_SPEC_FILE_PATH, 0);
	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		if (nvmeibt_topology_is_disk_explicitly_auto_takeover(&stock_local_disk->from_config.native_serial, stock_local_disk->from_config.vendor, stock_local_disk->from_config.smart_info.Model, stock_local_disk->from_config.nsid)) {
			stock_local_disk->is_auto_takeover = 1;
			continue;	// Do not report is_auto_takeover stock local disks before takeover (bind to nvmeibs)
		}
	}
	NFOUT;
	return rv;
}

/**
 * Prints the managment configuration to the given file.
 *
 * @author max (3/6/17)
 *
 * @param fptr - destination where to write the management
 *  		   configuration.
 */
void nvmeibt_read_config_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_Buf		wire_config;
	struct mm_mgmt_conf 	*conf;
	struct HW_mgmt_conf 	*HW_mgmt_conf = nvmeibt_global_get_global()->HW_mgmt_conf;

	NFIN;
	wire_config.buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full,
																					  TLV_TYPE_TOPO_CONFIG_COMPLETE, (char **)&(wire_config.data_buf));
	(*printf_fn)(printf_ctx, "\n- - - - -   MANAGEMENT CONFIG   - - - - -\n");
	if (!HW_mgmt_conf) {
		(*printf_fn)(printf_ctx, "HW configuration is empty\n");
	} else {
		HW_print_conf(HW_mgmt_conf, printf_fn, printf_ctx);
	}
	(*printf_fn)(printf_ctx, "-------------------------------------------------\n");
	if (wire_config.buf_len == 0) {
		(*printf_fn)(printf_ctx, "Volumes configuration is empty\n");
	} else {
		conf = mm_wire_buf_to_mm_mgmt_conf(wire_config.data_buf, 0, NULL);
		if (conf) {
			mm_print_conf(conf, printf_fn, printf_ctx);
			mm_conf_free_tree(conf);
		}
		else {
			(*printf_fn)(printf_ctx, "ERROR: bad wire configuration\n");
		}
	}
	NFOUT;
}

