#include <sys/stat.h>
#include "nvmeibt_node.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_disk.h"
#include "nvmeibt_global.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_rpc.h"
#include "../common/nvmeib_hash.h"

#include "nvmeibt_topology.h"
#include "nvmeibt_kafka.h"
#include "interfaces/log/nvmeibt_binary_tracing.h"

static struct nvmeibt_topology *_global_ctx_ptr = NULL;
#define global_ctx (*_global_ctx_ptr)
int64_t nvmeibt_follower_keep_alive_secs = MGMT_KEEP_ALIVE_SECS_DEFAULT;
int64_t nvmeibt_leader_keep_alive_secs = MGMT_LEADER_KEEP_ALIVE_SECS_DEFAULT;

const char	toma_persistency_file_name[] = NVMEIBT_PERSISTENCY_CACHE_DIR"toma_persistence_2_8_0_raft_and_topo.0";

struct nvmeibt_topology *nvmeibt_global_get_global(void)
{
	return &global_ctx;
}

struct nvmeibt_global_adaptive_timeouts_ctx		my_nvmeibt_global_adaptive_timeouts;

void nvmeibt_global_init(void)
{
	struct timespec						tmp_timespec;

	NFIN;
	_global_ctx_ptr = calloc(1, sizeof(*_global_ctx_ptr));
	global_ctx.persistent_toma_software_version = TOMA_SW_COMPATIBILITY_VER;
	global_ctx.mgmt_DB_uuid = nvmeib_uuid_null_val;
	getnstimeofday_boot(&(global_ctx.startup_timespec));
	getnstimeofday_convert_boot_to_real(&global_ctx.startup_timespec, &tmp_timespec);
	global_ctx.startup_timestamp_msec = timespec_to_msec(tmp_timespec);		// Don't use timespec_to_nsec() as MGMT will round the LSBs
	global_ctx.block_devices_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw1, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 3), "block_devices_hash", 16);
	global_ctx.nics_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw3, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 2), "nics_hash", 16);
	global_ctx.disks_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw4, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 2), "disks_hash", 16);
	global_ctx.disk_segments_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw2, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 6), "disks_segments_hash", 16);
	global_ctx.praids_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw5, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 4), "praids_hash", 16);
	global_ctx.nodes_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw6, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "nodes_hash", 16);
	global_ctx.chunks_hash_by_uuid = NVMEIB_HASH_CREATE(vhghnw7, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 2), "chunks_hash", 16);
	global_ctx.clients_hash_by_cid = NVMEIB_HASH_CREATE(vhghnw8, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 6), "clients_hash", 4);
	global_ctx.nvmesh_local_disks_hash_by_ldisk_id_str = NVMEIB_HASH_CREATE(vhghnw9, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "local_disks_hash", -1);
	global_ctx.stock_local_disks_hash_by_ldisk_id_str = NVMEIB_HASH_CREATE(vhghnwa, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "stock_local_disks_hash", -1);
	global_ctx.formatting_local_disks_hash_by_ldisk_id_str = NVMEIB_HASH_CREATE(vhghnws, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "formatting_local_disks_hash", -1);
	global_ctx.local_nics_hash_by_sw_gid_str = NVMEIB_HASH_CREATE(vhghnwd, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "local_nics_hash", -1);

	XDLIST_HEAD_INIT(&(global_ctx.registrants_on_invalid_seg));
	XDLIST_HEAD_INIT(&(global_ctx.excluded_drives_spec));
	XDLIST_HEAD_INIT(&(global_ctx.auto_takeover_drives_spec));
	XDLIST_HEAD_INIT(&(global_ctx.udev_events_info));
	XDLIST_HEAD_INIT(&(global_ctx.seg_active_post_update_action_list));
	XDLIST_HEAD_INIT(&(global_ctx.immediate_report_to_mgmt_praid_list));
	XDLIST_HEAD_INIT(&(global_ctx.praid_topo_recalc_list));
	NVMEIBT_BUF_INIT(&(global_ctx.buf_of_follower_wire_topo));

	global_ctx.is_serialize_active_topo_for_leader = true;
	//global_ctx.is_global_shutdown = 0;
	global_ctx.raft_pause_mode = RAFT_NOT_PAUSED;
	global_ctx.n_stores_in_progress = 0; // Init number of stores in the air to 0.
	global_ctx.running_report_target_ID = 0;
	global_ctx.is_valid_topo_config_received = 0;
	global_ctx.last_sent_report_target_ID = 0;
	global_ctx.config_tag = 1;
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(y4gs28j);	// Upon wake up we wish to report at least once our current state.
	nvmeibt_global_set_raft_pause_mode(RAFT_NOT_PAUSED);
	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();	// Generate the initial buffers
	//
	N_Tf(rty7499, "\n"
		"NVMEIBT_MAX_:\n"
		"	CSV_LINE_LENGTH="MACRO_DEF_TO_STR(NVMEIBT_MAX_CSV_LINE_LENGTH)"\n"
		"	NAME_LENGTH="MACRO_DEF_TO_STR(NVMEIBT_MAX_NAME_LENGTH)"\n"
		"	N_NODES="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_NODES)"\n"
		"	N_BLOCK_DEVICES="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_BLOCK_DEVICES)"\n"
		"	N_NICS_PER_NODE="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_NICS_PER_NODE)"\n"
		"	N_NICS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_NICS)"\n"
		"	N_DISKS_PER_NODE="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_DISKS_PER_NODE)"\n"
		"	N_DISKS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_DISKS)"\n"
		"	N_DISK_SEGMENTS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_DISK_SEGMENTS)"\n"
		"	STRIPE_WIDTH_PER_CHUNK="MACRO_DEF_TO_STR(NVMEIBT_MAX_STRIPE_WIDTH_PER_CHUNK)"\n"
		"	N_CHUNK_PER_BLOCK_DEVICE="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_CHUNK_PER_BLOCK_DEVICE)"\n"
		"	N_CHUNKS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_CHUNKS)"\n"
		"	N_CSV_LINES="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_CSV_LINES)"\n"
		"	N_CLIENTS_PER_NODE="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_CLIENTS_PER_NODE)"\n"
		"	N_CHUNKS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_CHUNKS)"\n"
		"	N_PRAIDS="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_PRAIDS)"\n"
		"	N_SEGMENTS_IN_PRAID="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_SEGMENTS_IN_PRAID)"\n"
		"	N_PORTS_PER_NIC="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_PORTS_PER_NIC)"\n"
		"	N_GIDS_PER_PORT="MACRO_DEF_TO_STR(NVMEIBT_MAX_N_GIDS_PER_PORT)"\n"
		"MAX_GLOBAL_TOPO_CSV_TEXT_LENGTH="MACRO_DEF_TO_STR(MAX_GLOBAL_TOPO_CSV_TEXT_LENGTH)"\n"
		 );
	N_Tf(eu8i832, "\n"
		"sizeof():\n"
		"	topology=@SIZEOF\n"
		"	raft_ctx=@SIZEOF\n"
		"	client=@SIZEOF\n"
		"	block_device=@SIZEOF\n"
		"	nic=@SIZEOF\n"
		"	disk=@SIZEOF\n"
		"	disk_segment=@SIZEOF\n"
		"	praid=@SIZEOF\n"
		"	node=@SIZEOF\n"
		"	chunk=@SIZEOF\n"
		"	csv_buf_ctx=@SIZEOF"
		,
		sizeof(struct nvmeibt_topology),
		sizeof(struct nvmeibt_raft_ctx),
		sizeof(struct nvmeibt_client),
		sizeof(struct nvmeibt_block_device),
		sizeof(struct nvmeibt_nic),
		sizeof(struct nvmeibt_disk),
		sizeof(struct nvmeibt_disk_segment),
		sizeof(struct nvmeibt_praid),
		sizeof(struct nvmeibt_node),
		sizeof(struct nvmeibt_chunk),
		sizeof(struct nvmeibt_Str)
		);
	NFOUT;
}

void nvmeibt_global_validate_and_upd_mgmt_DB_uuid(const union nvmeib_uuid *mgmt_DB_uuid)
{
	NFIN;
	if (ARE_UUID_EQ(mgmt_DB_uuid, &nvmeib_uuid_null_val)) {
		goto out;
	}
	if (!ARE_UUID_EQ(mgmt_DB_uuid, nvmeibt_global_get_mgmt_DB_uuid())) {
		if (!ARE_UUID_EQ(nvmeibt_global_get_mgmt_DB_uuid(), &nvmeib_uuid_null_val)) {
			N_ETf(4nsu2sa, "mgmt_DB_uuid mismatch @UUID_LE != @UUID_LE", mgmt_DB_uuid, nvmeibt_global_get_mgmt_DB_uuid());
			nvmeibt_abort(ES_FATAL);
		}
		*nvmeibt_global_get_mgmt_DB_uuid() = *mgmt_DB_uuid;
	}
out:
	NFOUT;
}

bool nvmeibt_replay_is_enabled(void);

int64_t nvmeibt_topology_max_praids_in_a_report = TOPOLOGY_MAX_PRAIDS_IN_A_REPORT_DEFAULT;
void nvmeibt_global_issue_leader_report_praids_status_to_mgmt(void)
{
	struct nvmeibt_praid				*praid;
	static struct nvmeibt_Str			*json_payload = NULL;
	int8_t								n_segs_written_praid;
	static int							n_segs_in_report_to_mgmt_total = 0;
	int64_t								n_praids_written_total;
	long long int						report_time_diff;
	struct timespec						now, start_timespec;
	int64_t								ns_since_start;

	if (!nvmeibt_raft_is_leader()) {
		goto skip_report_to_mgmt;
	}

	NFIN;

	if (XDLIST_N_ELEMNTS(&(global_ctx.immediate_report_to_mgmt_praid_list)) == 0) {
		N_Tf(w777nd2, "Nothing to report");
		goto skip_report_to_mgmt;
	}

	n_praids_written_total = 0;		// Even if we need to rereport the static json_payload
	// If not enough time since prev report then goto out
	report_time_diff = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_get_last_global_report_to_mgmt_timespec());
	if (report_time_diff < MAX_TIME_BETWEEN_CHANGED_SEGMENT_REPORTS_NSECS) {
		goto skip_report_to_mgmt;
	}
	getnstimeofday_boot(&start_timespec);
	/*
	 * During replay: skip reporting of any status to management:
	 *  - We only need to replay the leader's logic and how it calculates new
	 *    topologies based on config and follower reports.
	 *  - Decouple replay from the original execution environment (network)
	 *    where recording took place originally.
	 */
	if (nvmeibt_replay_is_enabled())
		goto skip_report_to_mgmt;
	if (json_payload) {
		if (n_segs_in_report_to_mgmt_total) {		// Unsent report
			N_Tf(5b38sdk, "Found an unsent report, resending it as is");
			goto send_json_payload;
		}
		nvmeibt_Str_reuse(json_payload);
	} else {
		json_payload = NNVMEIBT_STR_ALLOC(60dmgpi);
	}
	nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT_L
						"\"payload\": {"
						"\"pRaidsUpdate\": [",
						KAFKA_PRODUCER_MSG_HEADER_VAR_L("updatePRaidReport", 1));
	// Immediate reports
	XDLIST_FOREACH_SAFE(praid, &(global_ctx.immediate_report_to_mgmt_praid_list)) {
		getnstimeofday_boot(&now);
		ns_since_start = timespec_diff_ns(now, start_timespec);
		if (	((n_praids_written_total >= nvmeibt_topology_max_praids_in_a_report) ||
				 (ns_since_start > MSEC_TO_NSEC(10) && n_praids_written_total))) {
			N_Tf(uhwbks7, "stopping report at n_praids_written_total=@UINT64_TD ns_since_start=@LLD", n_praids_written_total, ns_since_start);
			break;
		}
		n_segs_written_praid = nvmeibt_praid_append_to_report_to_mgmt(praid, json_payload);
		if (n_segs_written_praid) {
			n_segs_in_report_to_mgmt_total += n_segs_written_praid;
			n_praids_written_total++;
		}
		XDLIST_DEL(&(praid->global_report_to_mgmt_praid_link));
	}
	N_Tf(ndswa03, "total: n_segs_written_total=@INT", n_segs_in_report_to_mgmt_total);
	// Complete the end of the json_payload
	if (n_praids_written_total) {
		nvmeibt_Str_chop_last_char(json_payload);
	} else {
		goto skip_report_to_mgmt;
	}
	nvmeibt_Str_sprintf(json_payload, "]}}");

send_json_payload:
	{
		//char 									unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
		//nvmeibt_strlcpy(unique_key, "praid_status", sizeof(unique_key) - strlen(unique_key));	TODO(Beware, if the report is partial it might delete older reports);
		// nvmeibt_strlcpy(unique_key + strlen(unique_key), nvmeibt_praid_id_str(praid), sizeof(unique_key) - strlen(unique_key));
		nvmeibt_kafka_outgoing_msgs_queue_add(NULL /*unique_key*/, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
		n_segs_in_report_to_mgmt_total = 0;	// Signify that no need to resend it
		nvmeibt_global_set_last_global_report_to_mgmt_timespec(nvmeibt_global_get_cur_event_start_time());
	}

skip_report_to_mgmt:
	// replay: end of skipped logic
	NFOUT;
}

void nvmeibt_global_leader_clear_old_reports_to_mgmt(void)
{
	struct nvmeibt_praid				*praid;

	NFIN;
	/* We want to delete only old and so not relevant reports.
	   If the praid versions in 'baseline' and 'to_report' lots are the same
	   it means, that we just didn't send those reports when being leader previous time.
	   A praid appears in the 'report_to_mgmt' list only after it's commited by majority,
	   thus we can send the report */
	XDLIST_FOREACH_SAFE(praid, &(global_ctx.immediate_report_to_mgmt_praid_list)) {
		if ((praid->praid_leader.to_report_praid_lot.topo_ctx.praid_version_major != praid->praid_leader.baseline_praid_lot.topo_ctx.praid_version_major) ||
			(praid->praid_leader.to_report_praid_lot.topo_ctx.praid_version_minor != praid->praid_leader.baseline_praid_lot.topo_ctx.praid_version_minor))
			XDLIST_DEL(&(praid->global_report_to_mgmt_praid_link));
	}
	NFOUT;
}

void nvmeibt_global_add_seg_active_post_update_action(struct nvmeibt_seg_active *seg_active)
{
	if (nvmeibt_seg_active_get_disk_segment(seg_active)) {
		XDLIST_ADD_TAIL(&(global_ctx.seg_active_post_update_action_list), seg_active);
	} else {
		N_Tf(y77u8u2, "seg_active=@UUID_8 is not a part of config, don't mark", nvmeibt_seg_active_UUID_8(seg_active));
	}
}

void nvmeibt_global_call_all_seg_active_post_update_actions(void)
{
	struct timespec							now;
	int64_t									time_diff_nsec;
	struct nvmeibt_seg_active				*seg_active;

	NFIN;
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Tf(tcvwsmn, "Skipping is_HW_config_functional=FALSE");
		goto out;
	}
	/* Don't use "XDLIST_FOREACH_SAFE" because after nvmeibt_seg_active_handle_post_update_actions call
	   the seg_active_post_update_action_list can be completely changed */
	for (int i = XDLIST_N_ELEMNTS(&(global_ctx.seg_active_post_update_action_list)); i > 0; i--) {
		getnstimeofday_boot(&now);
		time_diff_nsec = timespec_diff_ns(now, nvmeibt_global_get_cur_event_start_time());
		N_Tf(xnnkw55, "time from event start=@INT64 nsec", time_diff_nsec);
		if (time_diff_nsec * 5 > nvmeibt_raft_get_effective_heartbeat_timeout_ns() * 4) {
			N_Tf(xnnkw66, "post update actions time exceeded, continue later");
			break;
		}

		seg_active = XDLIST_FIRST(&(global_ctx.seg_active_post_update_action_list));
		if (seg_active == NULL)
			break;
		nvmeibt_seg_active_handle_post_update_actions(seg_active);
	}
out:
	NFOUT;
}

/******************************************************************************/

extern int64_t nvmeibt_toma_is_not_reporting_data_segs_gpt_entries;

static void write_gpt_entries_json(struct nvmeibt_local_disk *cur_local_disk, struct nvmeibt_Str *report_target)
{
	size_t											pre_array_fill_len;
	int												part_idx;
	char											part_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];
	struct nvmeibt_urn_uuid							mgmt_db_uuid_to_report;
	struct nvmeibt_urn_uuid							partition_uuid;
	char											owner[16];
	struct nvmeibt_disk_gpt_partition_entry			*cur_gpt_entry;
	union nvmeib_uuid								mgmt_db_uuid;
	BOOL											is_seg_found;
	BOOL											is_excelero_data_partition;
	struct nvmeibt_seg_active						*seg_active;

	NFIN;
	pre_array_fill_len = nvmeibt_Str_strlen(report_target);
	// GPT entries
	for (part_idx = 0; part_idx < cur_local_disk->main_gpt.max_n_entries; part_idx++) {
		cur_gpt_entry = &cur_local_disk->main_gpt.entries[part_idx];
		mgmt_db_uuid = nvmeib_uuid_null_val;
		is_seg_found = false;
		is_excelero_data_partition = false;

		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(cur_gpt_entry)) {
			continue;
		}
		if (	nvmeibt_toma_is_not_reporting_data_segs_gpt_entries &&
				nvmeibt_is_partition_type_seg_data_partition(&cur_gpt_entry->partition_type_guid)) {
			// As long as we do not support import of segs, mgmt knows about them
			continue;
		}
		seg_active = nvmeibt_global_get_seg_active_through_seg_by_uuid(&(cur_gpt_entry->partition_type_guid));
		if (seg_active) {
			is_seg_found = true;
			if (	!nvmeibt_seg_active_is_jbod(seg_active) &&
					!nvmeibt_seg_lot_is_deleted_in_config(nvmeibt_seg_active_get_applied_seg_lot(seg_active)) &&
					!nvmeibt_seg_active_is_conf_corrupted(seg_active)) {
				mgmt_db_uuid = seg_active->persistent_metadata->header.mgmt_db_uuid;
			} else { // JBOD has no metadata, so we don't know for sure it's mgmt_db_uuid
				mgmt_db_uuid = nvmeib_uuid_null_val;
			}
		} else {
			mgmt_db_uuid = nvmeib_uuid_null_val;
		}
		if (	ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_METADATA_PARTITION_TYPE_GUID) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID)) {
			sprintf(owner, "nvmesh");
			is_excelero_data_partition = true;
		} else {
			sprintf(owner, "system");
		}
#if NO_USE_FOR_THIS_INFO
		if (	ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_METADATA_PARTITION_TYPE_GUID) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID) ||
				ARE_UUID_EQ(&cur_gpt_entry->partition_type_guid,  &EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID)) {
			is_excelero_md_partition = true;
		}
#endif	// #if NO_USE_FOR_THIS_INFO
		char16_str_to_str(cur_gpt_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, part_name);
		mgmt_db_uuid_to_report = nvmeibt_union_uuid_to_urn_uuid(&mgmt_db_uuid);
		partition_uuid = nvmeibt_union_uuid_to_urn_uuid(&cur_gpt_entry->partition_guid);
		// If there is no segment but there is a gpt entry, we mark it as zeroed, this shouldn't happen generally so we issue an error but try to survive.
		if (!is_seg_found && !is_excelero_data_partition) {
			N_Wf(vxime6a, "gpt entry=@ENTRY_STR no seg found, assuming finished zeroing but not yet deleted..", partition_uuid.str);
		}
		nvmeibt_Str_sprintf(report_target,
						"{ \"partitionGuid\" : \"%.40s\", \"partitionType\" : \"%s\", \"partitionName\" : \"%s\", "
						"\"owner\" : \"%s\", \"start\" : %zu, \"end\" : %zu, \"mgmtDbUuid\" : \"%s\" },",
						partition_uuid.str,
						nvmeibt_disk_metadata_get_partition_type_name(cur_gpt_entry->partition_type_guid),
						part_name,
						owner,
						cur_gpt_entry->pba_s,
						cur_gpt_entry->pba_e,
						mgmt_db_uuid_to_report.str);

	}
	if (pre_array_fill_len != nvmeibt_Str_strlen(report_target)) {
		nvmeibt_Str_chop_last_char(report_target);	// Remove the last "," after the last gpt_entry report
	}
	NFOUT;
}

static void write_gpt_json(struct nvmeibt_local_disk *cur_local_disk, struct nvmeibt_Str *report_target)
{
	NFIN;
	nvmeibt_Str_sprintf(report_target,
							", \"GPT\" : { \"diskGuid\" : \"%s\", \"isValid\" : %d, \"firstUsableLba\" : %zu, "
							"\"lastUsableLba\" : %zu, \"mgmtDbUuid\" : \"%s\", \"maxNGptEntries\" : %u, \"entries\" : [",
							nvmeibt_union_uuid_to_urn_uuid(&cur_local_disk->main_gpt.header.disk_obj_uuid).str,
							/*cur_local_disk->main_gpt.is_valid ? 1 : 0*/ 1,
							align_pba_s_up_to_blkset( cur_local_disk->main_gpt.header.first_usable_pba, nvmeibt_local_disk_pblk_size(cur_local_disk)),
							align_pba_e_down_to_blkset(cur_local_disk->main_gpt.header.last_usable_pba, nvmeibt_local_disk_pblk_size(cur_local_disk)),
							nvmeibt_union_uuid_to_urn_uuid(&cur_local_disk->from_config.disk_metadata.mgmt_db_uuid).str,
							cur_local_disk->main_gpt.max_n_entries);
	write_gpt_entries_json(cur_local_disk, report_target);
	nvmeibt_Str_sprintf(report_target, "]}");
	NFOUT;
}

static void write_nics_json(struct nvmeibt_Str *report_target)
{
	struct nvmeibt_local_nic	*local_nic;
	size_t						pre_array_fill_len;

	NFIN;

	nvmeibt_Str_sprintf(report_target, "%s", "\"nics\": [");

	pre_array_fill_len = nvmeibt_Str_strlen(report_target);
	NVMEIB_HASH_FOREACH(local_nic, global_ctx.local_nics_hash_by_sw_gid_str) {
		nvmeibt_Str_sprintf(report_target,
							"{\"nicID\" : \"0x%s\", \"protocol\" : %d, \"status\" : %d, \"guid\" : \"0x%s\", \"pkey\" : \"%s\", \"pci_root\" : 0, \"mtu\" : %d, \"deviceType\" : \"%s\" },",
							local_nic->from_config.hw_gid_uuid.str,
							nvmeib_transport_cton(local_nic->from_config.link[0]),
							(strncmp(local_nic->from_config.state, "ACTIVE", sizeof("ACTIVE") - 1) == 0) ? 1 : 0,
							local_nic->from_config.sw_gid_uuid.str,
							local_nic->from_config.pkey,
							local_nic->from_config.mtu,
							local_nic->from_config.device_type);
	}

	if (pre_array_fill_len != nvmeibt_Str_strlen(report_target)) {
		nvmeibt_Str_chop_last_char(report_target); // Remove the last "," after the last nic report
	}

	nvmeibt_Str_sprintf(report_target, "%s", "]");

	NFOUT;
}

static void write_one_disk_json(
				struct nvmeibt_local_disk *local_disk,
				struct nvmeibt_Str *report_target)
{
	struct nvmeibt_local_disk_util_smart_info		*smart_info;
	char											disk_status[NVMEIBS_DISKS_CSV_STATUS_LEN];
	bool											is_gpt_report_needed;

    NFIN;

	if (!nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk)) {
		N_Tf(fkito09, "disk=@STR path=@PATH is_smart_log_valid=@BOOL is_done_reading_gpt=@BOOL is_mbr_a_valid_pmbr=@BOOL main_gpt.is_valid=@BOOL. skipping this disk in the report for now",
			nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_file_name(local_disk),
			local_disk->is_smart_log_valid, nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk), local_disk->is_mbr_a_valid_pmbr, local_disk->main_gpt.is_valid);
		goto out;
	}

 	/*
	 * We send the GPT of the disk only if it is not in the following states,
	 * in which the GPt does not exist, or is not guaranteed to be valid:
	 *   Not_Initialized - No GPT.
	 *   Formatting - Might be GPT, not guaranteed to exist, or to be written to disk.
	 *   Ingesting - We don't know the state of the GPt yet.
	 */
	is_gpt_report_needed = (!local_disk->is_excluded && (nvmeibt_local_disk_is_formatted(local_disk)));

	if (is_gpt_report_needed && !local_disk->main_gpt.is_valid) {
		N_Wf(m6tc1x9, "disk=@STR path=@PATH is_smart_log_valid=@BOOL is_done_reading_gpt=@BOOL is_mbr_a_valid_pmbr=@BOOL. GPT is not valid! skipping this disk in the report for now",
			nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_file_name(local_disk),
			local_disk->is_smart_log_valid, local_disk->is_done_reading_gpt_existing_or_not, local_disk->is_mbr_a_valid_pmbr);
		goto out;
	}

   smart_info = &local_disk->from_config.smart_info;

	if (smart_info->Serial_Number[0] == 0) {
		N_Wf(ki867vd, "disk=@STR path=@PATH has no serial number. skipping this disk in the report",
			nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_file_name(local_disk));
		goto out;
	}

	if ((strcmp(local_disk->from_config.status, "Ok") || local_disk->was_last_read_of_smart_counters_successful) &&
		(local_disk->serjio_status != NVMEIBS_SERJIO_STATUS_ERROR))
		snprintf(disk_status, sizeof(disk_status), "%s", local_disk->from_config.status);
	else
		snprintf(disk_status, sizeof(disk_status), "%s", "Error");

	nvmeibt_Str_sprintf(report_target,
						"{ \"diskID\" : \"%s\", \"disk_version\" : %u, \"blocks\" : %llu, \"block_size\" : %u, \"metadata_size\" : %u, "
						"\"pci_address\" : \"%s\", \"Serial_Number\" : \"%s\", \"nsid\": %d, \"Vendor\" : \"0x%x\", "
						"\"Model\" : \"%s\", \"Submission_Queues\" : %u, \"Completion_Queues\": %d, \"MSIX_Interrupts\" : %d, "
						"\"Numa_Node\" : %d, \"Critical_Warning\" : \"0x%x\", \"Available_Spare\" : \"%u_%%\", \"Available_Spare_Threshold\" : \"%u_%%\", "
						"\"Percentage_Used\" : \"%u_%%\", \"Controller_Busy_Time\" : \"0x%x\", \"Power_Cycles\" : \"0x%x\", \"Power_On_Hours\" : \"0x%x\", "
						"\"Unsafe_Shutdowns\" : \"0x%x\", \"Media_Errors\" : \"0x%x\", \"Number_of_Error_Information_Log_Entries\" : \"0x%llx\", "
						"\"status\" : \"%s\", \"isExcluded\" : %s, "
						"\"excludeReason\" : \"%s\", "
						"\"metadataCapabilities\" : \"%u\", "
						"\"formatOptions\" : %s, \"writeCounter\" : %llu, \"reappearingCounter\" : %u, \"formatRequestCounter\" : %u, "
						"\"activeFormatRequestCounter\" : %u",	// No terminating "}" since we might add GPT fields
							smart_info->diskID,
							local_disk->from_config.disk_metadata.disk_version_unused,
							smart_info->blocks,
							smart_info->block_size,
							smart_info->metadata_size,
							smart_info->Pci_Address,
							smart_info->Serial_Number,
							local_disk->from_config.nsid,
							smart_info->Vendor,
							smart_info->Model,
							smart_info->Submission_Queues,
							smart_info->Completion_Queues,
							smart_info->MSIX_Interrupts,
							smart_info->Numa_Node,
							smart_info->Critical_Warning,
							smart_info->Available_Spare,
							smart_info->Available_Spare_Threshold,
							smart_info->Percentage_Used,
							smart_info->Controller_Busy_Time,
							smart_info->Power_Cycles,
							smart_info->Power_On_Hours,
							smart_info->Unsafe_Shutdowns,
							smart_info->Media_Errors,
							smart_info->Number_of_Error_Information_Log_Entries,
							disk_status,
//							local_disk->from_config.status,
							local_disk->is_excluded ? "true" : "false",
							local_disk->is_excluded ? (local_disk->is_explicitly_excluded ? "Explicit" : "In-Use") : "None",
							smart_info->metadata_cap,
							smart_info->format_options,
							smart_info->Host_Write_Commands,
							local_disk->reappearing_counter,
							local_disk->from_config.disk_metadata.format_request_counter,
							nvmeibt_local_disk_get_active_format_request_counter_to_report(local_disk)
						);

	if (smart_info->format_options[0] == 0) {
		N_Ef(t_zz_234_zy, "Bad format_options!");
		nvmeibt_abort(ES_FATAL);
	}
	if (is_gpt_report_needed) {
		write_gpt_json(local_disk, report_target);
	}

	nvmeibt_Str_sprintf(report_target, "},");

out:
    NFOUT;
}

int64_t generic_block_device_support = GENERIC_BLOCK_DEVICE_SUPPORT_DEFAULT;

static void write_disks_json(struct nvmeibt_Str *report_target)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_local_disk	*stock_local_disk;
	size_t						pre_array_fill_len;

	NFIN;

	N_Tf(fjiut85, "Generating report n_local_disks=@N_LOCAL_DISKS n_stock_local_disks=@N_STOCK_LOCAL_DISKS",
		nvmeib_hash_get_n_elements(global_ctx.nvmesh_local_disks_hash_by_ldisk_id_str),
		nvmeib_hash_get_n_elements(global_ctx.stock_local_disks_hash_by_ldisk_id_str));

	nvmeibt_Str_sprintf(report_target, "%s", "\"disks\": [");

	pre_array_fill_len = nvmeibt_Str_strlen(report_target);
	NVMEIB_HASH_FOREACH(local_disk, global_ctx.nvmesh_local_disks_hash_by_ldisk_id_str) {
		if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
			continue;
		}
		if (nvmeibt_local_disk_is_bind_back_to_stock_needed(local_disk) || nvmeibt_local_disk_is_binding_back_to_stock(local_disk)) {
			N_Tf(sko95ft, "Skipping local disk=@STR in report_target since it is in the process of binding back to stock", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		if (	nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(local_disk), global_ctx.stock_local_disks_hash_by_ldisk_id_str) != NULL &&
				!is_supported_not_nvme_disk(local_disk)) {
			N_Wf(dlo0t65, "Skipping disk=@STR in report_yarget since it is found concurrently under both drivers, this is a transient state", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		write_one_disk_json(local_disk, report_target);
	}
	NVMEIB_HASH_FOREACH(stock_local_disk, global_ctx.stock_local_disks_hash_by_ldisk_id_str) {
		if (stock_local_disk->is_auto_takeover) {
			continue;	// Do not report is_auto_takeover stock local disks before takeover (bind to nvmeibs)
		}
		if (nvmeibt_local_disk_is_being_deleted(stock_local_disk)) {
			continue;
		}
		if (nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(stock_local_disk), global_ctx.nvmesh_local_disks_hash_by_ldisk_id_str) != NULL) {
			N_Wf(dlo0or5, "Skipping stock_disk=@STR in report_target since it is found concurrently under both drivers, this is a transient state", nvmeibt_local_disk_display(stock_local_disk));
			continue;
		}
		if (!generic_block_device_support && is_supported_not_nvme_disk(stock_local_disk)) {
			N_Tf(titi952, "Skipping stock_disk=@STR in report_target because non-NVMe drive support is off", nvmeibt_local_disk_display(stock_local_disk));
			continue;
		}
		write_one_disk_json(stock_local_disk, report_target);
	}
	if (nvmeib_hash_get_n_elements(global_ctx.formatting_local_disks_hash_by_ldisk_id_str)>0) {
		N_Tf(fju8334, "Adding to report @N_LOCAL_DISKS disks which are being formatted", nvmeib_hash_get_n_elements(global_ctx.formatting_local_disks_hash_by_ldisk_id_str));
		NVMEIB_HASH_FOREACH(local_disk, global_ctx.formatting_local_disks_hash_by_ldisk_id_str) {
			if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
				continue;
			}
			write_one_disk_json(local_disk, report_target);
		}
	}

	if (pre_array_fill_len != nvmeibt_Str_strlen(report_target)) {
		nvmeibt_Str_chop_last_char(report_target);	// Erase the disks terminating ","
	}

	nvmeibt_Str_sprintf(report_target, "%s", "]");
	NFOUT;
}

extern char toma_cfg_id[];
extern char toma_cfg_name[];
extern char toma_cfg_version[];

extern int nvmeibt_toma_report_target_max_between_secs;

static void send_report_target_if_needed(void)
{
	/*
	 * On every hardware change that affects the reporting, running_report_target_ID is increased
	 * (mgmt might miss some).
	 *   - In any case TOMA will not send more than one msg a second (sometimes wait, and accumulate changes)?
	 * - TOMA will send a new full report with the current reportID only upon resend_req from mgmt (No retries or logic on the TOMA side)
	 * - Only full reports are sent from TOMA, and only upon request.
	 * What triggers an increase in the reportID.
	 * - New/deleted/driver-change of disks
	 * - GPT changes
	 * - Smart changes (except for the writeCounter).
	 * - Zeroing changes
	 */

	struct nvmeibt_Str			*report_target = NULL;
	static unsigned int			max_report_target_len = 5000;
	struct timespec				now;
	struct timespec				diff_timeout;
	struct nvmeibt_local_disk	*local_disk;
	static struct nvmeibt_Str	*json_payload = NULL;

	NFIN;
	if (!nvmeibt_kafka_is_mgmt_zone_specified()) {
		N_Tf(3vsmfk6, "Skipping. !kafka_is_mgmt_zone_specified()");
		goto out;
	}
	if (global_ctx.last_sent_report_target_ID == global_ctx.running_report_target_ID) {
		N_Tf(rvsxkj3, "Skipping. Not need for report_target");
		goto out;
	}
	// We need to send a report, still we might defer it due to timeout reasons
	if (!json_payload) {
		json_payload = NNVMEIBT_STR_ALLOC(5jnsd9k);
	}

	getnstimeofday_boot(&now);
	diff_timeout = timespec_sub(now, global_ctx.last_local_report_target_to_mgmt_time);
	if (diff_timeout.tv_sec <= 1) {
		N_Tf(ianey6d, "Skipping. Waiting 1 sec after prev send");
		goto out;
	}


#	define MAX_WAIT_FOR_DISKS_ON_BOOT_SECS 15
	if (nvmeibt_global_get_cur_event_start_time().tv_sec - nvmeibt_global_get_startup_timespec().tv_sec < MAX_WAIT_FOR_DISKS_ON_BOOT_SECS) {
		NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
			if (!nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk)) {
				N_Tf(c724j49, "local_disk=@STR not done_reading. Avoiding partial report to mgmt", nvmeibt_local_disk_display(local_disk));
				goto out;
			}
		}
	}
	// Allocate report target buffer message.
	report_target = NNVMEIBT_STR_ALLOC(trace_toma_send_report_target_if_needed_2);
	NNVMEIBT_STR_RESIZE_BUF(error_toma_send_report_target_if_needed, report_target, max_report_target_len);
	//
	nvmeibt_Str_sprintf(report_target,  "{" KAFKA_PRODUCER_MSG_HEADER_FMT "\"payload\": {", KAFKA_PRODUCER_MSG_HEADER_VAR("reportTarget", 1));
	nvmeibt_Str_sprintf(report_target, "\"node\": {\"zone\": \"%lld\", \"bootTime\": %lld, \"cpu_temp\": \"30.0\", ",
						nvmeibt_kafka_get_kafka_mgmt_zone_number(), nvmeibt_global_get_startup_timestamp_msec());
	nvmeibt_Str_sprintf(report_target, "\"version\": \"%s\", \"buildNumber\": \"%s\", \"reportID\": %d, \"branch\": \"%s\", \"commit\": \"%s\", ",
						BUILD_VERSION_FOR_MGMT, BUILD_NUMBER_FOR_MGMT, global_ctx.running_report_target_ID, GIT_BRANCH, GIT_COMMIT_ID);
	nvmeibt_Str_sprintf(report_target,"\"configProfile\": {\"id\": \"%s\", \"name\": \"%s\", \"version\": \"%s\"}, ",
						toma_cfg_id, toma_cfg_name, toma_cfg_version);
	nvmeibt_Str_sprintf(report_target, "\"node_status\": \"1\", \"node_id\": \"%s\", \"targetUpdatesSequence\": %lld, \"cpu_load\": \"0.0\", ",
						nvmeibt_get_my_hostname(),
						RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
//		write_config_json(report_target);
	write_disks_json(report_target);
	nvmeibt_Str_sprintf(report_target, ", ");
	write_nics_json(report_target);
	max_report_target_len = max(max_report_target_len, nvmeibt_Str_strlen(report_target) + 10);
	nvmeibt_Str_sprintf(report_target, "}}}");

	//char unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
	//nvmeibt_strlcpy(unique_key, "report_target", sizeof(unique_key));
	//nvmeibt_strlcpy(unique_key + strlen(unique_key), nvmeibt_get_my_hostname(), sizeof(unique_key) - strlen(unique_key));
	nvmeibt_kafka_outgoing_msgs_queue_add(NULL /*unique_key*/, nvmeibt_Str_str(report_target), nvmeibt_Str_strlen(report_target) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
	global_ctx.last_local_report_target_to_mgmt_time = now;
	NVMEIBT_GLOBAL_CLEAR_REPORT_TARGET_HAS_NEW_DATA(iwkmzb2);
out:
	NNVMEIBT_STR_FREE(6cg3298, report_target);
	NFOUT;
}

static void bind_stock_local_disks_to_nvmeibs_if_needed(void)
{
	struct nvmeibt_local_disk *stock_local_disk;

	NFIN;
	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		if (	(nvmeibt_local_disk_is_bind_to_nvmeibs_needed(stock_local_disk) &&
				 !nvmeibt_local_disk_is_binding_to_nvmeibs(stock_local_disk) &&
				 !(stock_local_disk->controller->is_excluded))) {
			nvmeibt_local_disk_launch_bind_to_nvmeibs(stock_local_disk);
		}
	}
	NFOUT;
}

static void bind_local_disks_back_to_stock_if_needed(void)
{
	struct nvmeibt_local_disk *local_disk;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		if (	((nvmeibt_local_disk_is_bind_back_to_stock_needed(local_disk) &&
				  !nvmeibt_local_disk_is_binding_back_to_stock(local_disk)) ||
				 local_disk->controller->is_excluded)) {
			nvmeibt_local_disk_launch_bind_back_to_stock(local_disk);
		}
	}
	NFOUT;
}

static void periodic_reread_smart_counters_from_local_disks(void)
{
	struct nvmeibt_local_disk *local_disk;
	struct nvmeibt_local_disk *stock_local_disk;

    NFIN;

	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		if (stock_local_disk->is_excluded) {
			N_Df(lekfob2, "stock_local_disk=@STR is excluded, not launching periodic_reread_smart_counters",  nvmeibt_local_disk_display(stock_local_disk));
			continue;
		}
		nvmeibt_local_disk_launch_local_disk_periodic_reread_smart_counters_if_needed(stock_local_disk, 1);
	}

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		if (local_disk->is_excluded) {
			N_Tf(093bhja, "local_disk=@STR is excluded, not launching periodic_reread_smart_counters, disk will move back to stock driver anyway",  nvmeibt_local_disk_display(local_disk));
			continue;
		}
		nvmeibt_local_disk_launch_local_disk_periodic_reread_smart_counters_if_needed(local_disk, 0);
	}

    NFOUT;
}

static void garbage_collect_as_needed(void)
{
	bool		is_any_garbage_collected;
	bool		is_all_garbage_collected;
	bool		is_modified = 0;
	bool		is_done_fully = 1;

	NFIN;
	if (nvmeibt_global_get_global()->garbage_collected_applied_topo_config_idx == RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied)) {
		N_Tf(cvsu4jm, "No need (TOPO_CONFIG, follower_applied)=@INT64_TX", RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied));
		goto out;
	}
	nvmeibt_disk_segment_garbage_collect_old_segments(&is_any_garbage_collected, &is_all_garbage_collected);	// Mostly for replacement segs
	is_modified = (is_modified || is_any_garbage_collected);
	is_done_fully = (is_done_fully && is_all_garbage_collected);
	nvmeibt_block_devices_garbage_collect(&is_any_garbage_collected, &is_all_garbage_collected);
	is_modified = (is_modified || is_any_garbage_collected);
	is_done_fully = (is_done_fully && is_all_garbage_collected);

	if (is_modified) {
		N_Tf(gjii962, "Running setup_relationships again, because garbage collection removed something. And we had conf_corrupted");
		nvmeibt_topology_setup_relationships();
		// If a object was garbage_collected we cannot be sure, that during CONFIG_TAG_OUTDATED phase a new config and topo were serialized.
		// So, mark that we have to serialize them.
		if (nvmeibt_raft_is_leader()) {
			SET_RAFT_COMMIT_LIFECYCLE_VAL(f6ist6m, TOPO,        leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,        leader_to_commit) + 1);
			SET_RAFT_COMMIT_LIFECYCLE_VAL(mv9qjui, TOPO_CONFIG, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit) + 1);
			nvmeibt_topology_leader_mark_recalc_required();
		}
	}
    if (is_done_fully)
		nvmeibt_global_get_global()->garbage_collected_applied_topo_config_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied);
out:
	NFOUT;
}

/******************************************************************************/

static int n_valid_lines_in_str(const char *str)
{
	int		i;
	int		n = 1;	// To be on the safe side

	for (i = 0; str[i]; i++) {
		n += ((str[i] == '\n') && (str[i+1] != '#'));	// Cheat, Do not count lines that start with a '#' (comment)
	}
	return n;
}

static struct nvmeibt_Str	*nvmesh_conf_Str = NULL;
static struct nvmeibt_KVP	*nvmesh_conf_KVP_arr = NULL;
static int					n_entries_nvmesh_conf_KVP_arr;

int nvmeibt_global_nvmesh_conf_file_read(void) {
	const char *nvmesh_conf_file_name = TOMA_ROOT_DIR "etc/nvmesh/.nvmesh.conf";	// Note the we read the file that was generated by the MGMT agent, and not the manually edited "nvmesh.conf"
	int 						rv = -1;
	int		 					fd = -1;
	NFIN;
	if (access(nvmesh_conf_file_name, F_OK) != 0) {			// Verify that we are good with the file permissions
		N_ETf(vbha27k, "@STR, @AUTO_ERRNO", nvmesh_conf_file_name);
		goto out;
	}
	if (nvmesh_conf_Str) {
		nvmeibt_Str_reuse(nvmesh_conf_Str);
	} else {
		nvmesh_conf_Str = NNVMEIBT_STR_ALLOC(aj5izh6);
	}
	rv = 1;	// Changed
	// Read the persistency file
	fd = NNVMEIBT_OPEN_READ(7dnytwf, nvmesh_conf_file_name, 1);
	if (fd < 0) {
		N_ETf(fks92ks, "Error while opening the file @STR. @AUTO_ERRNO", nvmesh_conf_file_name);
		rv = -1;
		goto out;
	}
	NNVMEIBT_STR_FREAD(9r3jedi, nvmesh_conf_Str, fd);
	if (nvmeibt_Str_strlen(nvmesh_conf_Str) < 100) {
		N_Ef(vmdi392, "Unreasonable len=@SIZE_T", nvmeibt_Str_strlen(nvmesh_conf_Str));
		rv = -1;
		goto out;
	} else {
		const int n_lines = n_valid_lines_in_str(nvmeibt_Str_str(nvmesh_conf_Str));
		N_Tf(8sj64ik, "@STR length = @SIZE_T n_valid_lines=@INT", nvmesh_conf_file_name, nvmeibt_Str_strlen(nvmesh_conf_Str), n_lines);
		nvmesh_conf_KVP_arr = NNVMEIBT_TOMA_REALLOC(wsijtl4, nvmesh_conf_KVP_arr, n_lines * sizeof(struct nvmeibt_KVP));
		n_entries_nvmesh_conf_KVP_arr = nvmeibt_tokenize_KVP((char *)nvmeibt_Str_str(nvmesh_conf_Str), nvmeibt_Str_strlen(nvmesh_conf_Str), nvmesh_conf_KVP_arr, n_lines);
	}
out:
	if (fd >= 0) {
		NNVMEIBT_CLOSE(54vs93o, fd);
	}
	NFOUT;
	return rv;
}

const char *nvmeibt_global_nvmesh_conf_get_val_by_key(const char *key) {
	const size_t key_len = strlen(key);
	int	i;
	for (i = 0; i < n_entries_nvmesh_conf_KVP_arr; i++) {
		const struct nvmeibt_KVP *kvp = &nvmesh_conf_KVP_arr[i];
		if ((key_len == kvp->key_len) && !strncmp(key, kvp->key, kvp->key_len)) {
			// N_Tf(cvr876jvu7yh45ui, "key=@STR val=@STR", kvp->key, kvp->val);
			return kvp->val;
		}
	}
	return NULL;
}

/******************************************************************************/

static volatile bool is_reread_nvmesh_conf_required = 1;	// For the first read
void nvmeibt_global_mark_is_reread_nvmesh_conf_required(void)
{
	is_reread_nvmesh_conf_required = 1;
}

int nvmeibt_global_reread_nvmesh_conf_as_needed(void)
{
	int		rv = 0;
	int		rv_all = 0;

	if (!is_reread_nvmesh_conf_required) {
		goto out;
	}
	rv = nvmeibt_global_nvmesh_conf_file_read();
	if (rv == 1) {
		nvmeibt_kafka_upd_from_nvmesh_conf();
	} else if (rv < 0) {
		rv_all = rv;
	}
	rv = nvmeibt_read_excluded_target_drives();
	if (rv < 0) {
		rv_all = rv;
	}
	rv = nvmeibt_read_auto_takeover_target_drives();
	if (rv < 0) {
		rv_all = rv;
	}
	is_reread_nvmesh_conf_required = 0;
out:
	return rv_all;
}

void nvmeibt_global_idle_time_activities(void)
{
	static int64_t		n_calls = 0;
	const int64_t		n_calls_at_artificial_full_log = 10;
	NFIN;
	if (n_calls >= n_calls_at_artificial_full_log) {
		if (n_calls == n_calls_at_artificial_full_log) {
			log_snapshotting_set_active_log_levels("Restore");		// We started with a temporary "+ all"
		}
		update_traces();
	}
	n_calls++;
	read_rpc_config_from_persist(false);
	nvmeibt_global_reread_nvmesh_conf_as_needed();
	//
	if (nvmeibt_global_get_global()->is_in_shutdown_active_phase) {
		goto out;
	}
	// New topology were absorbed, Launch recoveries as needed.
	nvmeibt_recovery_execute_cold_recoveries_as_needed();
	nvmeibt_recovery_execute_dirty_rebuilds_as_needed();
	nvmeibt_global_issue_leader_report_praids_status_to_mgmt();
	nvmeibt_recovery_report_rebuild_progress_to_mgmt();

	send_report_target_if_needed();
	bind_stock_local_disks_to_nvmeibs_if_needed();
	bind_local_disks_back_to_stock_if_needed();
	nvmeibt_toma_process_waiting_udev_events();
	periodic_reread_smart_counters_from_local_disks();
	garbage_collect_as_needed();
	nvmeibt_recovery_execute_stale_and_txid_rebuilds_as_needed();
	nvmeibt_recovery_execute_JGC_rebuilds_as_needed();
	nvmeibt_recovery_execute_scrubbing_as_needed();
	nvmeibt_validate_alloc_free_summary_table();
	nvmeibt_register_validate_n_active_vs_n_applied();
	nvmeibt_server_lib_consume_incomming_srvr_msgs();
	nvmeibt_rpc_run();
	nvmeibt_wq_stuck_pthread_check();
	nvmeibt_global_call_all_seg_active_post_update_actions();
	nvmeib_hash_resize_all_tables_as_needed();
	//
	nvmeibt_raft_calc_timeouts_based_on_IIRs();
	nvmeibt_raft_follower_upd_effective_raft_heartbeat_timeout_and_factor();
	//
	nvmeibt_global_log_snapshotting_heuristic();
out:
	NFOUT;
}

void nvmeibt_global_set_raft_pause_mode(enum raft_pause_mode_enm pause_mode)
{
	if (pause_mode != global_ctx.raft_pause_mode) {
		switch (pause_mode) {
		case RAFT_NOT_PAUSED:
			N_Tf(4bhs934, "RAFT processing resumed");
			break;
		case RAFT_IN_PAUSED:
			N_Tf(mvcislq, "RAFT processing discard incoming messages");
			break;
		case RAFT_OUT_PAUSED:
			N_Tf(rnjskla, "RAFT processing discard outgoing messages");
			break;
		default: /* IN + OUT */
			N_Tf(7cbdhsk, "RAFT processing discard all messages");
		}
		global_ctx.raft_pause_mode = pause_mode;
	}
}

static int64_t		nvmeibt_log_snapshotting_mode_start_timestamp_sec = 0;
static int64_t 		prev_snapshotting_timestamp_sec= 0;

void nvmeibt_global_set_log_snapshotting_mode(int64_t is_log_snapshotting_mode)
{
	struct timespec		now;
	int64_t				val;

	// is_log_snapshotting_mode=-1 --> disabled.
	// is_log_snapshotting_mode=0/1 --> can be modified by log_snapshotting_heuristic()
	if (nvmeibt_log_snapshotting_mode_start_timestamp_sec == is_log_snapshotting_mode) {
		N_Tf(cvbsj3o, "No change - Skipping");
	} else if (is_log_snapshotting_mode == -1LL) {
		N_IMf(bshukw9, "Disabling log_snappshoting_mode");
		nvmeibt_log_snapshotting_mode_start_timestamp_sec = -1ULL;
	} else if (is_log_snapshotting_mode) {     // 1(==now) or time-stamp
		getnstimeofday_boot(&now);
		if (nvmeibt_log_snapshotting_mode_start_timestamp_sec > 0) {
			N_Tf(bfh39sl, "Skipping. log_snappshoting_mode is already active for @LLD secs.", now.tv_sec - nvmeibt_log_snapshotting_mode_start_timestamp_sec);
		} else {
			val = (is_log_snapshotting_mode == 1 ? now.tv_sec : is_log_snapshotting_mode);      // "1" means use the current time, otherwise (e.g. from persist) already a timestamp
			N_IMf(4vs0l2m, "Turning on log_snappshoting_mode @INT64_TD", val);
			nvmeibt_log_snapshotting_mode_start_timestamp_sec = val;
		}
	} else {	// 0
		N_IMf(7vbgwsq, "Turning off log_snappshoting_mode");
		nvmeibt_log_snapshotting_mode_start_timestamp_sec = 0;
	}
}

int64_t nvmeibt_global_get_log_snapshotting_mode(void)		// Return the timestamp. Primarily for persistence
{
	return nvmeibt_log_snapshotting_mode_start_timestamp_sec;
}

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_core_file_new(void)
{
	DIR					*d = NULL;
	struct dirent		*entry;
	char				core_full_path[256];
	uint32_t			newest_core_timestamp_sec = 0;
	const char			systemd_coredump_dir[] = TOMA_ROOT_DIR "var/lib/systemd/coredump/";
	const char			ubuntu_coredump_dir[] =  TOMA_ROOT_DIR "var/lib/apport/coredump/";
	size_t				dir_name_len;
	struct stat			st;
	const char			leader_file_name[] = TOMA_LOG_DIR "/toma_leader_name";
	time_t				leader_file_timestamp_sec = 0;
	bool				is_new = 0;
	int					rc;

	NFIN;
	// Get the newest_core_timestamp_sec
	// Unfortunatelly, there is no simple generic method to locate the coredump directory
	d = opendir(systemd_coredump_dir);
	if (d) {
		dir_name_len = strlen(systemd_coredump_dir);
		nvmeibt_strlcpy(core_full_path, systemd_coredump_dir, sizeof(systemd_coredump_dir));
	} else {
		d = opendir(ubuntu_coredump_dir);
		dir_name_len = strlen(ubuntu_coredump_dir);
		nvmeibt_strlcpy(core_full_path, ubuntu_coredump_dir, sizeof(ubuntu_coredump_dir));
	}
	if (d) {
		N_Tf(csjh8sk, "coredump directory=@STR", core_full_path);
	} else {
		N_Ef(r9wj3kx, "No coredump directory (@STR, @STR)", systemd_coredump_dir, ubuntu_coredump_dir);
		goto out;
	}
	// Get the newest_core_timestamp_sec
	while ((entry = readdir(d)) != NULL) {
		if (entry->d_type != DT_REG) {		// is regular file?
			continue;
		}
		if (strncmp(entry->d_name, "core.", 5) != 0) {
			continue;
		}
		nvmeibt_strlcpy(core_full_path + dir_name_len, entry->d_name, sizeof(core_full_path) - dir_name_len);
		rc = stat(core_full_path, &st);
		if (st.st_mtim.tv_sec < newest_core_timestamp_sec) {
			continue;
		}
		newest_core_timestamp_sec = st.st_mtim.tv_sec;
		N_Tf(3coklsm, "newest core_file=@STR", core_full_path);
	}
	if (newest_core_timestamp_sec == 0) {
		N_Tf(wir9bsw, "No core");
		goto out;
	}
	// Now that we have the core's date, decide whether is_new
	if (nvmeibt_global_get_cur_event_start_time().tv_sec - newest_core_timestamp_sec < (2 * 24 * 3600)) {
		N_Tf(2okex6z, "core=@STR is_new @LLD sec old", core_full_path, nvmeibt_global_get_cur_event_start_time().tv_sec - newest_core_timestamp_sec);
		is_new = 1;
	}
	// Try to detect whether the last TOMA run ended-up with a core dump
	// Using the date of the leader_file as a referrence.
	// Another (invalid) option would be to use the latest update in the trace_daemon directory,
	//  but we already wrote to the log, so it is not a working design.
	// compare with the leader_file_timestamp_sec
	rc = stat(leader_file_name, &st);
	if (rc < 0) {
		N_Tf(usnklwl, "No @STR", leader_file_name);
		goto out;
	}
	leader_file_timestamp_sec = st.st_mtime;
	if (newest_core_timestamp_sec > leader_file_timestamp_sec) {
		N_Tf(df5wyh2, "core file is newer than leader_file");
		is_new = 1;
	}
out:
	if (d) {
		closedir(d);
	}
	NFOUT;
	return is_new;
}

static int n_toma_restarts_in_the_last_5_days(void)
{
	char		cmd[256];
	char		n_restarts_file_name[64];
	char		n_as_str[10] = {0};
	int			fd = 0;
	int			rv = 0;
	int			n_bytes_read;
	int			system_status;
	snprintf(n_restarts_file_name, sizeof(n_restarts_file_name), TOMA_ROOT_DIR "tmp/jctl_%d", getpid());
	snprintf(cmd, sizeof(cmd), TOMA_BINLOG_DIR "/pager " TOMA_BINLOG_DIR " -l toma.eter.binlog -t now-120h --nogreet -f 'trace=trace_toma_nvmeibt_toma_init' | wc -l > %s", n_restarts_file_name);
	// Todo: Consider using faster code instead: snprintf(cmd, sizeof(cmd), "find " TOMA_ROOT_DIR " -name toma.binlog_marker* -mmin -7200 | wc -l > %s", n_restarts_file_name);
	N_Tf(0kkdoks, "@STR", cmd);
	system_status = system(cmd);
	if (!WIFEXITED(system_status) || WEXITSTATUS(system_status)) {
		N_Wf(xctajhq, "cmd=@STR: WIFEXITED=@INT WEXITSTATUS=@INT @AUTO_ERRNO", cmd, WIFEXITED(system_status), (int8_t)WEXITSTATUS(system_status));
		goto out;
	}
	fd = NNVMEIBT_OPEN_READ(n5sko9v, n_restarts_file_name, 1);
	if (fd < 0) {
		N_Ef(34isc1q, "Error opening file=@STR @AUTO_ERRNO", n_restarts_file_name);
		goto out;
	}
	if ((n_bytes_read = NNVMEIBT_PREAD(lixme5n, fd, n_as_str, sizeof(n_as_str), 0, false)) < 0) {
		N_Ef(4uxomwk, "Error while reading the file=@FILE, @AUTO_ERRNO", n_restarts_file_name);
		goto out;
	}
	rv = atoi(n_as_str);
out:
	unlink(n_restarts_file_name);	// Erase the file
	NNVMEIBT_CLOSE(a4i29ls, fd);
	N_Tf(cuanwk2, "n=@INT", rv);
	return rv;
}

/************************  logs_snapshotting_WQ  ******************************/

static bool is_logs_snapshotting_slowpath_wq_in_the_air = 0;	// Not atomic becuases accesses only from Toma main thread
struct logs_snapshotting_slowpath_wq_entry {
	struct nvmeibt_wq_entry 		wq_entry;
	bool							is_first_run_after_boot;
	bool							is_calling_snapshot;
	bool							is_starting_snapshotting;
	bool							is_stopping_snapshotting;
	struct nvmeibt_Str				*reason_for_snapshot;
	int								rv;
};

int nvmeibt_rpc_command_config(int argc, char *argv[], struct nvmeibt_Str *out);
void log_snapshotting_set_active_log_levels(char const *level)
{
	char					*argv_snapshotting[] = {"dummy", "log_snapshotting_mode", "-"};
	struct nvmeibt_Str		*err_msg = NNVMEIBT_STR_ALLOC(nuah8w4);
	uint64_t				nvmeibt_debug_level = -1ULL;
	static char				prev_level[] = "Unknown...";

	NFIN;
	if (strncmp(level, prev_level, sizeof(prev_level)) == 0) {
		goto out;
	}
	nvmeibt_strlcpy(prev_level, level, sizeof(prev_level));
	//
	N_Tf(suekslw, "level=@STR", level);
	if (strcmp(level, "High") == 0) {
		update_traces_turn_all_on_or_off('+', 1);
		argv_snapshotting[2]= "1";		// Overide the "-"
		nvmeibt_debug_level = 5;
	} else if (strcmp(level, "Restore") == 0) {
		argv_snapshotting[2] = "0";		// Overide the "-". Actually, Setting to low, and not restoring
		nvmeibt_debug_level = -1ULL;	// Restore to requested debug level
	} else {
		N_Ef(ish2ks8, "level=@STR", level);
		goto out;
	}
	nvmeibt_rpc_command_config(3, argv_snapshotting, err_msg);
	nvmeibt_binary_tracing_enforce_active_tracer_nvmeibt_debug_level(nvmeibt_debug_level);
	N_Tf(vwtya2d, "@STR=@STR debug_level=@UINT64_TD, @STR", argv_snapshotting[1], argv_snapshotting[2], nvmeibt_debug_level, nvmeibt_Str_str(err_msg));
out:
	NNVMEIBT_STR_FREE(xbgw8n4, err_msg);
	NFOUT;
}

static void logs_snapshotting_slowpath_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct logs_snapshotting_slowpath_wq_entry	*entry;
	int											system_status;
	char										log_snapshotting_script_cmd_line[512];

	NFIN;
	entry = container_of(wq_entry, struct logs_snapshotting_slowpath_wq_entry, wq_entry);
	if (nvmeibt_toma_is_in_shutdown()) {
		N_Tf(trace_logs_snapshotting_slowpath_wrapper_shutdown, "TOMA is in shutdown. Skipping snapshotting");
		entry->rv = -1;
		goto out;
	}
	if (entry->is_first_run_after_boot) {
		entry->is_stopping_snapshotting = 1;	// Unless we will/already turn on is_starting_snapshotting, "restore" trace.config
		// Check if this was a crash
		// Look for a core file, and see if it is newer than leader_file and/or less than 2 days old
		if (is_core_file_new()) {
			entry->is_starting_snapshotting = 1;
			nvmeibt_Str_strcat(entry->reason_for_snapshot, "==is_core_file_new");
		}
		if (n_toma_restarts_in_the_last_5_days() > 10) {
			entry->is_starting_snapshotting = 1;
			nvmeibt_Str_strcat(entry->reason_for_snapshot, "==n_toma_restarts_in_the_last_5_days_gt_10");
		}
		entry->is_calling_snapshot = 1;
	}
	if (nvmeibt_global_get_log_snapshotting_mode() == -1LL) {
		N_Tf(3vw8ssl, "log_snapshotting_mode=-1. I.e., DISABLED. Skipping all actions");
		entry->rv = 0;
		goto out;
	}
	if (nvmeibt_global_get_cur_event_start_time().tv_sec - prev_snapshotting_timestamp_sec < (10 * 60)) {	// No more than every 10 min (reboot overrides it)
		entry->is_calling_snapshot = 0;
	}
	if (entry->is_calling_snapshot) {
		N_Tf(sunkq9s, "Calling nvmesh_snapshot_logs.sh");
		snprintf(log_snapshotting_script_cmd_line, sizeof(log_snapshotting_script_cmd_line),
				 TOMA_ROOT_DIR "opt/nvmesh/common-repo/scripts/nvmesh_snapshot_logs.sh --toma --reason %.400s", nvmeibt_Str_str(entry->reason_for_snapshot));
		system_status = system(log_snapshotting_script_cmd_line);
		if (!WIFEXITED(system_status) || WEXITSTATUS(system_status)) {
			entry->rv = (int8_t)WEXITSTATUS(system_status);
			N_Wf(cva7u2o, "nvmesh_snapshot_logs.sh: WIFEXITED=@INT WEXITSTATUS=@INT @AUTO_ERRNO", WIFEXITED(system_status), (int8_t)WEXITSTATUS(system_status));
			goto out;
		}
	}
	entry->rv = 0;
out:
	// Control the logging level
	if (entry->is_starting_snapshotting) {
		log_snapshotting_set_active_log_levels("High");
	} else if (entry->is_stopping_snapshotting) {
		log_snapshotting_set_active_log_levels("Restore");
	}
	nvmeibt_toma_trigger_wakeup_handle_err(wq_entry);
	NFOUT;
}

static void logs_snapshotting_slowpath_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct logs_snapshotting_slowpath_wq_entry		*entry;

	NFIN;
	entry = container_of(wq_entry, struct logs_snapshotting_slowpath_wq_entry, wq_entry);
	if (wq_entry->is_canceled) {
		entry->rv = -1;
	}
	if (entry->rv < 0) {
		N_Wf(xcghasl, "Error logs_snapshotting_slowpath rv=@INT", entry->rv);
	}
	if (entry->is_calling_snapshot) {
		prev_snapshotting_timestamp_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;
	}

	NFOUT;
}

static void logs_snapshotting_slowpath_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct logs_snapshotting_slowpath_wq_entry *entry = container_of(wq_entry, struct logs_snapshotting_slowpath_wq_entry, wq_entry);
	NNVMEIBT_STR_FREE(598dnjw, entry->reason_for_snapshot);
	NNVMEIBT_BM_FREE(cbyshwj, entry);
	is_logs_snapshotting_slowpath_wq_in_the_air = 0;
}

static void nvmeibt_logs_snapshotting_slowpath_WQ_launch(bool is_calling_snapshot, bool is_starting_snapshotting, bool is_stopping_snapshotting, bool is_first_run_after_boot, struct nvmeibt_Str *reason_for_snapshot)
{
	struct logs_snapshotting_slowpath_wq_entry 	*work;

	NFIN;
	if (is_logs_snapshotting_slowpath_wq_in_the_air) {
		N_Tf(7lrtbyz, "Skipping. is_logs_snapshotting_slowpath_wq_in_the_air=1");
		goto out;
	}
	work = NNVMEIBT_BM_CALLOC(yshw93k, sizeof(*work));
	work->reason_for_snapshot = NNVMEIBT_STR_ALLOC(iajdrc6);
	nvmeibt_Str_clone(work->reason_for_snapshot, reason_for_snapshot);
	work->wq_entry.execute = logs_snapshotting_slowpath_wrapper;
	work->wq_entry.finalize = logs_snapshotting_slowpath_finalize;
	//
	work->wq_entry.type = "LOGS_SNAPSHOTTING_SLOWPATH";
	work->wq_entry.free = logs_snapshotting_slowpath_freer;
	//
	work->is_calling_snapshot = is_calling_snapshot;
	work->is_starting_snapshotting = is_starting_snapshotting;
	work->is_stopping_snapshotting = is_stopping_snapshotting;
	work->is_first_run_after_boot = is_first_run_after_boot;
	//
	is_logs_snapshotting_slowpath_wq_in_the_air = 1;
	if (nvmeibt_wq_run_once(&work->wq_entry) == NULL) {
		N_Ef(8xkwlpx, "Unable to add logs_snapshotting_slowpath task to WQ!");
		is_logs_snapshotting_slowpath_wq_in_the_air = 0;
		logs_snapshotting_slowpath_finalize(&(work->wq_entry));
		logs_snapshotting_slowpath_freer(&(work->wq_entry));
	}
out:
	NFOUT;
}

void nvmeibt_log_snapshotting_shutdown(void)
{
	int i;
	for (i = 0; (i < 20) && is_logs_snapshotting_slowpath_wq_in_the_air; i++) {
		N_Tf(rftsikl, "Awaiting for the running task to end");
		nanosleep(&(struct timespec){0, MSEC_TO_NSEC(100)}, NULL); // 100ms
	}
	if (is_logs_snapshotting_slowpath_wq_in_the_air) {
		N_Ef(rftsikk, "Work still running, not waiting anymore, application may crash...");
	}
}

/************************  logs_snapshotting_WQ end  **************************/

int nvmeibt_rpc_command_config(int argc, char *argv[], struct nvmeibt_Str *out);

void nvmeibt_global_log_snapshotting_heuristic(void)
{
	// Should be called periodically, and/or on timeout
	static uint32_t				prev_periodic_logic_timestamp_sec = 0;
	bool						is_calling_snapshot = 0;
	bool						is_starting_snapshotting = 0;
	bool						is_stopping_snapshotting = 0;
	bool						is_first_run_after_boot = (prev_periodic_logic_timestamp_sec == 0);
	static bool					is_leader_without_2_3rds_majority = 0;
	static bool					is_without_leader = 0;	// This "negative" name matches nvmeibt_raft_get_time_without_leader_sec()
	static bool					is_short_avg_leader_lifespan = 0;
	struct nvmeibt_Str			*reason_for_snapshot = NULL;

	NFIN;
	if (nvmeibt_global_get_cur_event_start_time().tv_sec - prev_periodic_logic_timestamp_sec < 10) {
		goto out;
	}
	reason_for_snapshot = NNVMEIBT_STR_ALLOC(5nkkmqs);
	prev_periodic_logic_timestamp_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;

	if (is_first_run_after_boot) {
		N_Tf(akwok42, "is_first_run_after_boot");
		nvmeibt_Str_strcat(reason_for_snapshot, "==is_first_run_after_boot");
		is_calling_snapshot = 1;
	}
	// If I am the leader, and more than 1/3 of the followers are down for more than 5 min
	if (nvmeibt_raft_is_leader()) {
		if ((nvmeibt_global_get_cur_event_start_time().tv_sec - nvmeibt_raft_get_leader_last_2_3rds_majority_timestamp_sec()) > (5 * 60)) {
			if (is_leader_without_2_3rds_majority) {
				// Nothing new
			} else {
				N_Tf(crasgh4, "last_2_3rds_majority_timestamp too old");
				is_starting_snapshotting = 1;
			}
			nvmeibt_Str_strcat(reason_for_snapshot, "==last_2_3rds_majority_timestamp_too_old");
			is_leader_without_2_3rds_majority = 1;
		} else {
			is_leader_without_2_3rds_majority = 0;
		}
	} else {
		is_leader_without_2_3rds_majority = 0;
	}
	if (nvmeibt_raft_get_time_without_leader_sec() > (5 * 60)) {
		if (is_without_leader) {
			// Nothing new
		} else {
			N_Tf(vw7i39p, "time_without_leader_sec=@INT", nvmeibt_raft_get_time_without_leader_sec());
			nvmeibt_Str_strcat(reason_for_snapshot, "==time_without_leader_too_long");
			is_starting_snapshotting = 1;
		}
		is_without_leader = 1;
	} else {
		is_without_leader = 0;
	}
	if (nvmeibt_raft_get_avg_leader_lifespan_sec() < 3600) {
		if (is_short_avg_leader_lifespan) {
			// Nothing new
		} else {
			N_Tf(owny5zj, "avg_leader_lifespan=@INT", nvmeibt_raft_get_avg_leader_lifespan_sec());
			nvmeibt_Str_strcat(reason_for_snapshot, "==avg_leader_lifespan_too_short");
			is_starting_snapshotting = 1;
		}
		is_short_avg_leader_lifespan = 1;
	} else {
		is_short_avg_leader_lifespan = 0;
	}
	if (nvmeibt_global_get_log_snapshotting_mode()) {	// Possibly is_stopping_snapshotting
		if (nvmeibt_global_get_cur_event_start_time().tv_sec - nvmeibt_log_snapshotting_mode_start_timestamp_sec > (5 * 24 * 3600)) {		// Turn off five days after starting snapshotting
			N_Tf(ainz3n7, "Stopping snapshotting - No reason recently");
			is_stopping_snapshotting = 1;
		}
	}
	is_calling_snapshot |= (is_starting_snapshotting && (nvmeibt_global_get_log_snapshotting_mode() == 0));
	if (is_calling_snapshot || is_starting_snapshotting || is_stopping_snapshotting || is_first_run_after_boot) {
		// We only get here on boot (where we need do always do some things), and/or if one of the quick tests yielded something
		nvmeibt_logs_snapshotting_slowpath_WQ_launch(is_calling_snapshot, is_starting_snapshotting, is_stopping_snapshotting, is_first_run_after_boot, reason_for_snapshot);
	}
out:
	NNVMEIBT_STR_FREE(u4ndiqp, reason_for_snapshot);
	NFOUT;
}

