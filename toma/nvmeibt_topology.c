#include "clnt/nvmeibt_client.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_common.h"
#include "interfaces/network/network_incs.h"
#include "nvmeibt_important_logs.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_persistency_info.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"
#include "nvmeibt_topo_bin.h"
#include "common/nvmeib_types.h"
#include "nvmeibt_kafka.h"
#include <sys/stat.h>
#include "nvmeibt_global.h"
#include "nvmeibt_seg_active.h"
extern struct nvmeibt_nm_local_node *nw_node;

enum replacement_action_t {
	ACTION_REPLACE = 14,
	ACTION_DONT_REPLACE,
	ACTION_ILLEGAL
};

struct disk_event_wq_entry {
	struct nvmeibt_wq_entry 	wq_entry;
	struct nvmeibs_toma_disk_change_msg disk_change_msg;
};

static const char *nvmeibs_toma_server_msg_type_to_str(
	const struct nvmeibs_toma_server_proc_buf *msg_buf)
{
	const unsigned char	*b = (typeof(b))msg_buf->buf;
	static char		output_buf[128];						// Daniel: very bad idea to do it like that

	switch (msg_buf->type) {
	case NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_REQ:		return "NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_REQ";
	case NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_RSP: 		return "NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_RSP";

	case NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE:			return "NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE";
	case NVMEIBS_TOMA_REPORT_EVENT_SUBSCRIBER_CHANGE:	return "NVMEIBS_TOMA_REPORT_EVENT_SUBSCRIBER_CHANGE";
	case NVMEIBS_TOMA_REPORT_EVENT_CLIENT_DISCONNECT:	return "NVMEIBS_TOMA_REPORT_EVENT_CLIENT_DISCONNECT";
	case NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED:	return "NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED";
	default:
		snprintf(output_buf, sizeof(output_buf), "msg_type=%x. %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
			msg_buf->type, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
		return output_buf;
	}
}

int64_t nvmeibt_topology_freeze_topo = 0;
#define NLOCAL_SERVER_MSG_DUMP(name, m) do {								\
	N_Tf(name, "@BUFFER_DUMP", nvmeibs_toma_server_msg_type_to_str(m));		\
} while (0)

static int raft_long_msg_test_appendix_len = 0;
#define RAFT_LONG_MSG_TEST_SINGLE_STR_LEN			128
static struct nvmeibt_Buf raft_long_msg_test_buf;

void nvmeibt_topology_print_versions(struct nvmeibt_topology_serialized_topo_header *header_ptr)
{
	N_Tf(u87er42, "sw_ver(@X<-->@X) TOPO(follower_applied=@INT64_TX follower_committed=@INT64_TX) TOOP_CONFIG(applied=@INT64_TX follower_committed=@INT64_TX)",
		 TOMA_SW_COMPATIBILITY_VER, header_ptr->sw_ver,
		 RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed),
		 RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
}

// nvmeibt_topology_upd_applied_mgmt_config_version() was replaced by nvmeibt_global_parse_MGMT_CONFIG_VERSION()

BOOL nvmeibt_topology_leader_is_recalc_required(void)
{
	return nvmeibt_global_get_global()->is_leader_regeneration_required;
}

void nvmeibt_topology_leader_mark_recalc_required(void)
{
	N_Tf(kityo95, "");
	nvmeibt_global_get_global()->is_leader_regeneration_required = 1;
}

void nvmeibt_topology_leader_clear_recalc_required(void)
{
	N_Tf(dki9223, "");
	nvmeibt_global_get_global()->is_leader_regeneration_required = 0;
}

BOOL nvmeibt_topology_active_is_reserialization_required(void)
{
	return nvmeibt_global_get_global()->is_active_reserialization_required;
}

void nvmeibt_topology_active_mark_reserialization_required(void)
{
	N_Tf(u8i83d6, "");
	nvmeibt_global_get_global()->is_active_reserialization_required = 1;
}

void nvmeibt_topology_active_clear_reserialization_required(void)
{
	N_Tf(t7y202d, "");
	nvmeibt_global_get_global()->is_active_reserialization_required = 0;
}

enum nvmeibt_topology_shutdown_state nvmeibt_topology_applied_get_shutdown_state(void)
{
	return nvmeibt_global_get_global()->shutdown_state;
}

void nvmeibt_topology_applied_mark_shutdown_start(void)
{
	N_Tf(t_s5_tomatopo, "");
	nvmeibt_topology_active_mark_reserialization_required();
	nvmeibt_global_get_global()->shutdown_state = NVMEIBT_SHUTDOWN_FIRST;
}

void nvmeibt_topology_applied_mark_shutdown(void)
{
	N_Tf(t_s6_tomatopo, "");
	nvmeibt_global_get_global()->shutdown_state = NVMEIBT_SHUTDOWN;
}

/********************     Disk & Node relationships     **********************/

void remove_all_disk_segments_from_remote_applied(struct nvmeibt_disk *disk)
{
	int									i;
	struct nvmeibt_disk_segment			*seg;

	N_Tf(ki9112j, "disk=@UUID_LE", nvmeibt_disk_UUID(disk));
	for (i = 0; i < disk->n_segments; i++) {
		seg = disk->disk_segments[i];

		nvmeibt_seg_remote_reset(&(seg->seg_leader.remote_seg_topo), seg);
		seg->seg_leader.is_removed_from_remote_applied = 1;
		NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(kd12091, nvmeibt_disk_segment_get_praid(seg));
	}
}

void nvmeibt_topology_remove_disk_from_its_current_node(struct nvmeibt_disk *disk)
{
	int					i;
	struct nvmeibt_node	*node;

	NFIN;
	if (disk) {
		node = disk->its_node_config;
		if (node) {
			N_Tf(kiujh76, "ldisk=@STR disk=@UUID_LE node=@NODE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_disk_UUID(disk), nvmeibt_node_name(node));
			for (i = node->n_disks_config - 1; i >= 0; --i) {
				if (node->disks_config[i] == disk) {
					node->disks_config[i] = node->disks_config[--(node->n_disks_config)];
					node->disks_config[node->n_disks_config] = NULL;
				}
			}
		}
		N_Tf(awe45b9, "its_node of ldisk=@STR set to NULL", nvmeibt_disk_get_ldisk_id_str(disk));
		disk->its_node_config = NULL;
		nvmeibt_local_disk_stop_all_activities(disk);

		nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(disk->its_local_disk);
	}
	NFOUT;
}

void nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(struct nvmeibt_disk *disk)
{
	int								i;
	struct nvmeibt_raft_member		*member = (disk ? disk->leader_its_raft_member : NULL);

	NFIN;
	if (!member) {
		goto out;
	}

	/* dumper: log this remove-disk-from-node event */
	nvmeibt_dumper_event_remove_disk(disk);

	N_Tf(hu86cf4, "ldisk=@STR disk=@UUID_LE node=@NODE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_disk_UUID(disk), nvmeibt_raft_member_name(member));
	for (i = member->n_disks_leader - 1; i >= 0; --i) {
		if (member->disks_leader[i] == disk) {
			N_Tf(trace_1_topology_nvmeibt_topology_leader_remove_disk_from_its_current_node, "Removed ldisk=@STR from node=@NODE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_raft_member_name(member));
			member->disks_leader[i] = member->disks_leader[--(member->n_disks_leader)];
			member->disks_leader[(member->n_disks_leader)] = NULL;
		}
	}

	N_Tf(r6t5tv3, "its_node of ldisk=@STR set to NULL", nvmeibt_disk_get_ldisk_id_str(disk));
	disk->leader_its_raft_member = NULL;
	remove_all_disk_segments_from_remote_applied(disk);
	nvmeibt_global_get_global()->should_send_segment_report = true;
out:
	NFOUT;
}

static void leader_add_disk_to_member(struct nvmeibt_raft_member *member, struct nvmeibt_disk *disk)
{
	int 	i;

	NFIN;
	if (!disk) {
		goto out;
	}

	if (disk->leader_its_raft_member != NULL) {
		N_Ef(utrw038, "Disk already attached to nodeid=@UUID_LE", nvmeibt_raft_member_id(disk->leader_its_raft_member));
		goto out;
	}
	disk->leader_its_raft_member = member;
	N_Tf(j87hy75, "ldisk=@STR disk=@UUID_LE node=@NODE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_disk_UUID(disk), nvmeibt_raft_member_name(member));
	nvmeibt_global_get_global()->should_send_segment_report = true;
	if (member) {
		int j;
		for (j = 0; j < member->n_disks_leader; ++j) {
			if (ARE_UUID_EQ(nvmeibt_disk_UUID(member->disks_leader[j]), nvmeibt_disk_UUID(disk))) {
				N_Ef(k7shwe, "ldisk=@STR is already in disks_leader of node=@NODE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_raft_member_name(member));
				goto out;
			}
		}
		member->disks_leader[member->n_disks_leader++] = disk;
	}
	for (i = 0; i < disk->n_segments; i++) {
		struct nvmeibt_disk_segment	*disk_segment = disk->disk_segments[i];
		N_Tf(gg77hy5, "seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(kohu993, nvmeibt_disk_segment_get_praid(disk_segment));
	}
out:
	NFOUT;
}

void nvmeibt_topology_leader_connect_disk_with_raft_member(struct nvmeibt_disk *disk, struct nvmeibt_raft_member *member)
{
	NFIN;
	if (!disk) {
		N_Tf(sr410nh, "disk=NULL");
		goto out;
	}
	if (disk->leader_its_raft_member == member) {
		N_Tf(lo0xk0y, "Nothing new");
		goto out;
	}
	if (disk->leader_its_raft_member) {
		nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(disk);
	}
	leader_add_disk_to_member(member, disk);
out:
	NFOUT;
}

void nvmeibt_topology_leader_detach_all_disks_from_raft_member(struct nvmeibt_raft_member *member)
{
	int									i;
	struct nvmeibt_node					*node;

	NFIN;
	if (member) {
		for (i = member->n_disks_leader - 1; i >= 0; --i) {
			nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(member->disks_leader[i]);
		}
		node = member->its_node;
		if (node) {
			for (i = node->n_disks_config - 1; i >= 0; --i) {
				remove_all_disk_segments_from_remote_applied(node->disks_config[i]);
			}
		}
	}
	NFOUT;
}

void nvmeibt_topology_detach_all_disks_from_node(struct nvmeibt_node *node)
{
	int		i;

	NFIN;
	for (i = node->n_disks_config - 1; i >= 0; --i) {
		nvmeibt_topology_remove_disk_from_its_current_node(node->disks_config[i]);
	}
	NFOUT;
}

void nvmeibt_topology_detach_all_nics_from_node(struct nvmeibt_node *node)
{
	int		i;

	NFIN;
	for (i = node->n_nics - 1; i >= 0; --i) {
		node->nics[i]->its_node = NULL;
	}
	node->n_nics = 0;
	NFOUT;
}

static void remove_ptr_from_disk_to_local_disks_that_are_missing(void)
{
	int 						i;
	struct nvmeibt_node			*my_node = nvmeibt_global_get_my_node();
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_disk			*disk;

	NFIN;
	if (!my_node) {
		N_Tf(icnaalw, "Too early, my_node=NULL");
		goto out;
	}
	// Clean the previous disk->its_local_disk and local_disk->its_disk
	for (i = my_node->n_disks_config - 1; i >= 0; --i) {
		disk = my_node->disks_config[i];
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_disk_get_ldisk_id(disk), &(nvmeibt_global_get_global()->local_disks_hash));
		if (!local_disk) {
			local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_disk_get_ldisk_id(disk), &(nvmeibt_global_get_global()->stock_local_disks_hash));
			if (!local_disk) {
				local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_disk_get_ldisk_id(disk), &(nvmeibt_global_get_global()->formatting_local_disks_hash));
			}
		}
		if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
			local_disk = NULL;
		}
		N_Tf(ted8xcj, "disk=@STR is_local_disk=@BOOL", nvmeibt_disk_get_ldisk_id(disk)->str, !!local_disk);
		if (!local_disk) {
			if (disk->its_local_disk) {
				disk->its_local_disk->its_disk = NULL;
				disk->its_local_disk = NULL;
			}
		}
	}
out:
	NFOUT;
}

/***************    Setup the relationships between entities    ******************/

static int connect_nics_with_nodes(void)
{
	struct nvmeibt_node		*node;
	struct nvmeibt_nic		*nic;
	int		rv = 0;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;

	XHASHTABLE_FOR_EACH_SAFE(nic, &cur_topo->nics_hash) {
		if (nic->its_node && ARE_UUID_EQ(nvmeibt_node_UUID(nic->its_node), &(nic->from_config.its_node_id))) {
			continue;
		}
		nvmeibt_nic_detach_from_node(nic);
		XHASHTABLE_FOR_EACH_SAFE(node, &cur_topo->nodes_hash) {
			if (ARE_UUID_EQ(nvmeibt_node_UUID(node), &(nic->from_config.its_node_id))) {
				nic->its_node = node;
				if (node->n_nics >= NVMEIBT_MAX_N_NICS_PER_NODE) {
					N_Ef(rt54ew0, "Too many nics for node_id=@UUID_LE nic=@UUID_LE", nvmeibt_node_UUID(node), nvmeibt_nic_UUID(nic));
					rv = -1;
					continue;
				}
				node->nics[node->n_nics++] = nic;
			}
		}
		if (!nic->its_node) {
			N_Ef(j9j8j9y, "Failed to find node for NIC @UUID_LE", nvmeibt_nic_UUID(nic));
			rv = -1;
			continue;
		}
	}
	NFOUT;
	rv = 0;
	return rv;
}

int nvmeibt_topology_add_seg_active_to_both_mem_gpts(struct nvmeibt_local_disk *local_disk, struct nvmeibt_seg_active *seg_active)
{
	int												rv = -1;
	uint64_t 										seg_active_metadata_size;
	struct nvmeibt_disk_gpt_partition_entry			*seg_metadata_gpt_entry;
	int												factor;
	struct nvmeibt_disk_segment						*disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	int												SEG_UUID_8 = nvmeibt_seg_active_UUID_8(seg_active);

	NFIN;

	TODO(Go over all the validations. Duplicated in various layers. Here only seg_level, not GPT/disk level. Maybe first validate both GPTs);

	seg_metadata_gpt_entry = nvmeibt_seg_active_get_metadata_gpt_entry(seg_active);
	if (seg_metadata_gpt_entry) {	// This is the common case. Quick check and we are done
		N_Tf(bskijw2, "seg=@UUID_8 Found existing entry. No harm", SEG_UUID_8);
		rv = 0;
		goto out;
	}
	// Now we know that it is does not have a GPT entry
	if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
		N_Tf(t63920e, "seg=@UUID_8 will not be added to gpt of disk=@STR. !is_ready_for_segments",
			 SEG_UUID_8, nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	if (local_disk->from_config.pblk_size > SECTOR_SIZE) {
		N_Ef(sy76510, "local_disk->from_config.pblk_size=@PBLK_SIZE", local_disk->from_config.pblk_size);
		rv = 1;
		goto out;
	}

	if ((local_disk->main_gpt.n_entries_in_use >= local_disk->main_gpt.max_n_entries) ||
		(local_disk->metadata_gpt.n_entries_in_use >= local_disk->metadata_gpt.max_n_entries)) {
		N_Ef(t_zzz_40,"Not enough space in GPTs seg=@UUID_8 disk=@STR, main GPT: @N_ACTIVE(@N_TOTAL) metadata GPT: @N_ACTIVE(@N_TOTAL)",
			 SEG_UUID_8,
			 nvmeibt_local_disk_display(local_disk),
			 local_disk->main_gpt.n_entries_in_use,
			 local_disk->main_gpt.max_n_entries,
			 local_disk->metadata_gpt.n_entries_in_use,
			 local_disk->metadata_gpt.max_n_entries);
		goto out;
	}

	// Find space for the disk segment metadata, in the metadata partition.
	seg_active_metadata_size = nvmeibt_ds_metadata_size_in_disk_pblks(disk_segment);
	seg_metadata_gpt_entry = nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(&local_disk->metadata_gpt,
																seg_active_metadata_size,
																local_disk->from_config.pblk_size,
																&EXCELERO_SEGMENT_METADATA_PARTITION_TYPE_GUID,
																nvmeibt_seg_active_UUID(seg_active),
																nvmeibt_seg_active_id_str(seg_active),
																URN_UUID_STR_LENGTH,
																NVMEIBR_PARTITION_ALIGNMENT_4KB);
	if (!seg_metadata_gpt_entry) {
		N_Ef(fhfh7gf, "Unable to allocate metadata partition entry on disk=@STR for seg=@UUID_8",
			 nvmeibt_local_disk_display(local_disk), SEG_UUID_8);
		goto out;
	}
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(dfsfd42, local_disk);

	// Calc the seg->lb_[se]_phys_blk
	factor = (SECTOR_SIZE / local_disk->from_config.pblk_size);	// Strong assumption: "factor >= 1"

	disk_segment->seg_mgmt.pba_s = disk_segment->seg_mgmt.lb_s * factor;
	disk_segment->seg_mgmt.pba_e = (disk_segment->seg_mgmt.lb_e + 1) * factor - 1;

	N_Tf(frfrt72, "Add seg=@UUID_8, uuid=@UUID_LE to the Main-GPT of disk=@STR",
		SEG_UUID_8,
		nvmeibt_seg_UUID(disk_segment),
		nvmeibt_local_disk_display(local_disk));
	// Add seg to MAIN GPT
	if (!nvmeibt_disk_metadata_add_mem_gpt_entry(&local_disk->main_gpt,
											(nvmeibt_seg_active_is_config_EC(seg_active) ?
											 &EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED : &EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL),
											nvmeibt_seg_UUID(disk_segment),
											disk_segment->seg_mgmt.pba_s,
											disk_segment->seg_mgmt.pba_e,
											nvmeibt_disk_segment_id_str(disk_segment),
											URN_UUID_STR_LENGTH)) {
		N_Ef(dyters5, "Unable to add seg=@UUID_8 to gpt of disk=@STR", SEG_UUID_8, nvmeibt_local_disk_display(local_disk));
		goto out;
	}

	nvmeibt_seg_active_upd_metadata_gpt_entry_and_ctrl(seg_active, seg_metadata_gpt_entry, NULL);
	rv = 0;
out:
	if (rv != 0) {
		char header[MGMT_LOG_MSG_HEADER_LEN];
		char msg[MGMT_LOG_MSG_MSG_LEN];

		N_Wf(hgrusha, "Unable to add seg=@UUID_8 to GPT on disk=@STR disk_status=@DISK_STATUS",
			 nvmeibt_seg_active_UUID_8(seg_active), nvmeibt_local_disk_display(local_disk), local_disk->from_config.status);
		// Send message to management that we evicted this disk.
		snprintf(header, sizeof(header), "disk %s is unusable", nvmeibt_local_disk_display(local_disk));
		snprintf(msg, sizeof(msg), "Unable to add seg %08x vol=%s to GPT of dev=%s", nvmeibt_seg_active_UUID_8(seg_active),
				 nvmeibt_seg_active_blkdev_name(seg_active), nvmeibt_local_disk_file_name(local_disk));
		nvmeibt_kafka_generic_log_msg_to_mgmt_send(NULL, header, msg, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
		if (rv > 0) {
			nvmeibt_seg_lot_mark_conf_corrupted(nvmeibt_seg_active_get_applied_seg_lot(seg_active));
			nvmeibt_seg_active_stop_all_recoveries_and_registrations(seg_active, 1);
		}
	}
	NFOUT;
	return rv;	// rv<0:Fail, rv==0:valid_entry, rv>0:permanent_fail(deleting ...)
}

#if 1
static int add_segments_to_local_disks_gpt(void)
{
	int								rv = 0;
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();

	NFIN;

	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
			N_Tf(sadrfv6, "disk=@STR. Not is_ready_for_segments yet. Skipping for now", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
			N_Tf(kiuht76, "Skipping local_disk=@STR. is_being_deleted", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		XHASHTABLE_FOR_EACH_SAFE(seg_active, &(local_disk->seg_active_hash)) {
			if (nvmeibt_seg_active_get_metadata_gpt_entry(seg_active)) {
				continue;	// Already in, at least in the memory copy, on its way to the disk
			}
			// Now we know that it is a missing local disk segment
			if (!nvmeibt_disk_segment_is_config_OK(nvmeibt_seg_active_get_disk_segment(seg_active))) {
				N_Wf(ku8hgt7, "seg=@UUID_8 !is_config_OK, not adding to GPT!", nvmeibt_seg_active_UUID_8(seg_active));
				continue;
			}
			// the following call must occure from "apply"
			nvmeibt_topology_add_seg_active_to_both_mem_gpts(local_disk, seg_active);
		}
	}

	NFOUT;
	return rv;
}
#endif

static void remove_seg_from_both_mem_gpts_if_eligable(struct nvmeibt_local_disk *local_disk, struct nvmeibt_disk_gpt_partition_entry *cur_gpt_entry)
{
	struct nvmeibt_seg_active				*seg_active;

	// NFIN;
	seg_active = nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(local_disk, &cur_gpt_entry->partition_guid);
	if (!seg_active) {
		N_Ef(b5ms93j, "Found seg=@UUID_LE on disk without seg_active", &cur_gpt_entry->partition_guid);
		nvmeibt_abort(ES_FATAL);
	}

	if (nvmeibt_seg_active_get_disk_segment(seg_active)) {
		N_Tf(vgd783n, "Skipping functioning seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}

	// Now we know that the seg_active is not a part of config and should be removed
	NVMEIBT_SEG_ACTIVE_FREE_MEM_AND_PROCESSES(seg_active);
	N_Tf(wtdhy43, "Deleting gpt_entry seg=@UUID_LE from disk=@STR", &cur_gpt_entry->partition_guid, nvmeibt_local_disk_display(local_disk));
	nvmeibt_disk_metadata_remove_entry_from_mem_gpt(&local_disk->main_gpt,     &cur_gpt_entry->partition_guid);
	nvmeibt_disk_metadata_remove_entry_from_mem_gpt(&local_disk->metadata_gpt, &cur_gpt_entry->partition_guid);
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(d9md4g5, local_disk);
out:
	// NFOUT;
	return;
}

static int trim_stale_gpt_entries_from_local_disks_gpt(void)
{
	struct nvmeibt_local_disk	*local_disk;
	int							rv = 0;

	NFIN;
	if (nvmeibt_global_get_global()->is_valid_topo_config_received) {
		XHASHTABLE_FOR_EACH_SAFE(local_disk, &nvmeibt_global_get_global()->local_disks_hash) {
			if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
				continue;	// Does not exist for us
			}
			if (!(local_disk->its_disk) || nvmeibt_disk_is_explicitly_out_of_service(local_disk->its_disk)) // out of service or not in config yet
				continue;
			for (int i = 0; i < local_disk->main_gpt.max_n_entries; i++) {
				struct nvmeibt_disk_gpt_partition_entry *cur_gpt_entry = &(local_disk->main_gpt.entries[i]);
				if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(cur_gpt_entry)) {
					continue;
				}
				if (nvmeibt_is_partition_type_seg_data_partition(&cur_gpt_entry->partition_type_guid)) {
					remove_seg_from_both_mem_gpts_if_eligable(local_disk, cur_gpt_entry);
				}
			}
		}
	} else {
		N_Tf(yryr773, "We don't have a config yet, skipping");
	}

	NFOUT;
	return rv;
}

static int connect_disks_with_disk_segments(void)
{
	struct nvmeibt_disk_segment *disk_segment;
	struct nvmeibt_disk			*disk;
	int							rv = 0;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;
	if (XHASHTABLE_N_ELEMENTS(&cur_topo->disks_hash) == 0) {
		N_Tf(bctavjw, "Too early. No disks (from HW_config) yet");
		goto out;
	}
	XHASHTABLE_FOR_EACH_SAFE(disk, &cur_topo->disks_hash) {
		disk->n_segments = 0;	// Reinitialize
	}
	XHASHTABLE_FOR_EACH_SAFE(disk_segment, &cur_topo->disk_segments_hash) {
		struct nvmeibt_disk			*prev_disk = disk_segment->seg_mgmt.its_disk;

		disk = NNVMEIBT_HASH_GET_OBJ_BY_UUID(dr56gr7, &cur_topo->disks_hash, &disk_segment->seg_mgmt.disk_id, disk);
		disk_segment->seg_mgmt.its_disk = disk;
		if (disk != NULL) {
			if (disk->n_segments >= disk->n_allocated_segments) { /* YR: should never be > */
				int n = disk->n_allocated_segments ? disk->n_allocated_segments << 1 : 16;
				disk->disk_segments = NNVMEIBT_TOMA_REALLOC(trace_1_topology_connect_disks_with_disk_segments, disk->disk_segments, n * sizeof(*disk->disk_segments));
				disk->n_allocated_segments = n;
			}
			disk->disk_segments[disk->n_segments++] = disk_segment;
		}

		if (prev_disk != disk_segment->seg_mgmt.its_disk) {
			if (!prev_disk) {
				continue;	// First time
			}
			if (disk_segment->seg_mgmt.its_disk) {
				N_Ef(adr5te3, "seg=@UUID_8 Moved disks from @STR to @STR", nvmeibt_seg_UUID_8(disk_segment),
					 nvmeibt_disk_get_ldisk_id_str(prev_disk), nvmeibt_disk_get_ldisk_id_str(disk_segment->seg_mgmt.its_disk));
				nvmeibt_disk_segment_mark_conf_corrupted(disk_segment);
				rv = -1;
				continue;
			}
			// its_disk turned to null
			N_Wf(oi98uht, "seg=@UUID_8 depr_flag=@DEPR_FLAG. ldisk=@STR disk=@UUID_LE was removed", nvmeibt_seg_UUID_8(disk_segment),
				 disk_segment->from_config.deprecation_flag, nvmeibt_disk_get_ldisk_id_str(prev_disk), nvmeibt_disk_UUID(prev_disk));
		}
		if (!disk_segment->seg_mgmt.its_disk && !nvmeibt_seg_lot_is_deleted_in_config(&disk_segment->seg_follower.applied_seg_lot)) {
			N_Wf(asdc12q, "Failed to find disk for disk_segment @UUID_8. Possibly evicted from HW config, but vol not deleted yet", nvmeibt_seg_UUID_8(disk_segment));
			rv = 1;
			continue;
		}
	}
out:
	NFOUT;
	return rv;
}

static int connect_local_disk_with_disk(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_disk			*disk;
	int							rv = 0;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	unsigned int				active_format_request_counter_to_report;

	N_Tf(8s73nsg, "local_disk=@STR", nvmeibt_local_disk_display(local_disk));
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(xcai4m6, "Skipping local_disk=@STR is_being_deleted", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	if (local_disk->is_excluded || !(local_disk->is_owned_by_nvmeibs_driver)) {
		N_Tf(bx94njw, "Skipping local_disk=@STR is_excluded=@BOOL is_owned_by_nvmeibs_driver=@BOOL", nvmeibt_local_disk_display(local_disk), local_disk->is_excluded, local_disk->is_owned_by_nvmeibs_driver);
		goto out;
	}
	if (nvmeibt_disk_get_local_disk(NNVMEIBT_LOCAL_DISK_GET_DISK(bchs03n, local_disk)) == local_disk) {
		N_Tf(0abxd2m, "local_disk=@STR is already connected, skipping it", nvmeibt_local_disk_display(local_disk));
		goto out;	// Already connected
	}
	disk = nvmeibt_disk_get_disk_by_ldisk_id(nvmeibt_local_disk_UUID(local_disk));
	if (disk) {
		active_format_request_counter_to_report = nvmeibt_local_disk_get_active_format_request_counter_to_report(local_disk);
		if (disk->from_config.active_format_request_counter < active_format_request_counter_to_report) {
			N_Tf(jqai4m6, "local_disk=@STR format_cnt(mgmt=@INT<disk=@INT), skipping",
				 nvmeibt_local_disk_display(local_disk), disk->from_config.active_format_request_counter, active_format_request_counter_to_report);
			goto out;
		}
		disk->its_local_disk = local_disk;
		local_disk->its_disk = disk;
		if (	ARE_UUID_EQ(&local_disk->from_config.disk_metadata.mgmt_db_uuid, &nvmeib_uuid_null_val) &&
				!ARE_UUID_EQ(&cur_topo->mgmt_DB_uuid, &nvmeib_uuid_null_val)) {
			N_Tf(i8i8u7a, "Updating mgmt_db_uuid of disk=@STR @UUID_LE-->@UUID_LE. Probably a newly formatted disk",
				nvmeibt_local_disk_display(local_disk), &local_disk->from_config.disk_metadata.mgmt_db_uuid, &cur_topo->mgmt_DB_uuid);
			local_disk->from_config.disk_metadata.mgmt_db_uuid = cur_topo->mgmt_DB_uuid;
		}
	}
	NNVMEIBT_LOCAL_DISK_GET_DISK(0kdl2aa, local_disk);	// Just for validation
out:
	return rv;
}

static int connect_local_disks_with_disks(void)
{
	struct nvmeibt_local_disk	*local_disk;
	int							rv = 0;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;
	remove_ptr_from_disk_to_local_disks_that_are_missing();
	N_Tf(asr5t46, "n_disks=@N_DISKS n_local_disks=@N_LOCAL_DISKS",
		NVMEIBT_HASH_N_OBJS(&cur_topo->disks_hash),
		NVMEIBT_HASH_N_OBJS(&cur_topo->local_disks_hash));

	// Connect all the local_disks to disks.
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		rv = connect_local_disk_with_disk(local_disk);
		if (rv < 0) {
			goto out;
		}
	}
/*
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->stock_local_disks_hash) {
		rv = connect_local_disk_with_disk(local_disk);
		if (rv < 0) {
			goto out;
		}
	}
*/
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->formatting_local_disks_hash) {
		rv = connect_local_disk_with_disk(local_disk);
		if (rv < 0) {
			goto out;
		}
	}
out:
	NFOUT;
	return rv;
}

static void connect_local_disk_segments_with_seg_active(void)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_disk				*disk;
	struct nvmeibt_disk_segment 	*seg;
//	struct nvmeibt_praid		 	*praid;
	struct nvmeibt_seg_active		*seg_active;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();
	union nvmeib_uuid				seg_uuid;
	int								seg_idx;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
			N_Tf(uu77bst, "disk=@STR is not ready for segs yet", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		disk = local_disk->its_disk;
		for (seg_idx = 0; seg_idx < disk->n_segments; seg_idx++) {
			seg = disk->disk_segments[seg_idx];
			seg_active = nvmeibt_disk_segment_get_seg_active(seg);
			if (!seg_active) {
				seg_uuid = *nvmeibt_seg_UUID(seg);
				seg_active = nvmeibt_find_seg_active_of_specific_local_disk_by_uuid(local_disk, &seg_uuid);
				if (!seg_active) {	// a new seg
					if (seg->seg_follower.committed_seg_lot.from_config.version != ILLEGAL_CONFIG_VER) {
						seg_active = nvmeibt_seg_active_create(&seg_uuid, local_disk, NULL, NULL);
					} else {
						continue;
					}
				}
				seg->seg_follower.seg_active = seg_active;
				seg_active->disk_segment = seg;
				nvmeibt_topology_add_seg_active_to_both_mem_gpts(local_disk, seg_active);
				nvmeibt_seg_active_we_got_its_seg(seg_active);
				NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(br3uygs, seg_active);	// Probably not needed (No topo change)
				nvmeibt_register_launch_seg_metadata_ctrl_save(seg_active);
			}
			else if (nvmeibt_disk_segment_is_deprecated_in_config(seg)) {
				NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(i87d04b, seg_active);
			}
		}
	}
	NFOUT;
}

static int connect_disk_with_node_by_config(void)
{
	int							rv = 0;
	struct nvmeibt_disk			*disk;
	struct nvmeibt_node			*node;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;

	// Redo from scratch
	XHASHTABLE_FOR_EACH_SAFE(node, &(cur_topo->nodes_hash)) {
		node->n_disks_config = 0;
	}
	XHASHTABLE_FOR_EACH_SAFE(disk, &(cur_topo->disks_hash)) {
		node = nvmeibt_node_get_node_by_id(&(disk->from_config.its_original_node_id));
		disk->its_node_config = node;
		if (!disk->its_node_config) {
			N_Wf(v46djws, "ldisk=@STR disk=@UUID_LE cannot locate its config node @UUID_LE", nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_disk_UUID(disk), &disk->from_config.its_original_node_id);
			continue;
		}
		node->disks_config[node->n_disks_config++] = disk;
	}
	NFOUT;
	return rv;
}

TODO(Move the following func to local_disk)
int update_liveliness_of_seg_actives_of_specific_local_disk(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_disk						*disk;
	int										rv = 0;	// Currently, never fails. Marks specific disk_segment as dead
	struct nvmeibt_seg_active				*seg_active;
	bool									new_is_ready_for_segments;

	NFIN;
	new_is_ready_for_segments = (nvmeibt_local_disk_is_ready_for_segments(local_disk) && nvmeibt_local_disk_is_connected_to_disk(local_disk));
	if (new_is_ready_for_segments == local_disk->prev_is_ready_for_segments) {
		N_Tf(wvmi5ve, "disk=@STR Unchanged is_ready_for_segments=@BOOL", nvmeibt_local_disk_display(local_disk), new_is_ready_for_segments);
		goto out;
	}
	disk = NNVMEIBT_LOCAL_DISK_GET_DISK(9vnwrs0, local_disk);
	if (!disk) {
		goto out;
	}
	if (disk->its_local_disk != local_disk) {
		N_Ef(ndhwy62, "ldisk=@STR its_local_disk=@PTR local_disk=@PTR)", nvmeibt_disk_get_ldisk_display(disk), disk->its_local_disk, local_disk);
		rv = -1;
		goto out;
	}
	N_Tf(cbdhyw0, "updating local_disk=@STR disk=@UUID_LE n_segs=@N_SEGS", nvmeibt_local_disk_display(local_disk), nvmeibt_disk_UUID(disk), disk->n_segments);
	local_disk->prev_is_ready_for_segments = new_is_ready_for_segments;
	XHASHTABLE_FOR_EACH_SAFE(seg_active, &(local_disk->seg_active_hash)) {
		nvmeibt_seg_active_upd_liveliness_according_to_local_disk(seg_active);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibt_topology_probe_local_hardware(struct nvmeibt_csv_file_ctx *config_files_list, unsigned int list_size)
{
	struct nvmeibt_Str			*new_config = NULL;
	unsigned int						i;
	int									rv = 0;

	NFIN;
	// Read the csv files
	new_config = NNVMEIBT_STR_ALLOC(aju6843);

	/*
	 * During replay: skip probing of local hardware from /proc:
	 *  - We only need to replay the leader's logic and how it calculates new
	 *    topologies based on config and follower reports.
	 *  - Decouple replay from the original execution environment (hardware)
	 *    where recording took place originally.
	 */
	if (!nvmeibt_replay_is_enabled()) {
		for (i = 0; i < list_size; i++) {
			if (nvmeibt_read_config_file(new_config, &(config_files_list[i])) < 0) {
				rv = -1;
				goto out;
			}
		}
		N_Tf(fju7865, "Local hardware changed");
	}
//	nvmeibt_topology_active_mark_reserialization_required();
	rv = nvmeibt_parse_csv_buf(new_config, 0, NVMEIBT_CSV_TYPE_LOCAL_DISKS);

out:
	NNVMEIBT_STR_FREE(ft67ut5, new_config);
	NFOUT;
	return rv;
}

static void mark_all_required_cleanups_for_all_local_segs_on_config_changes_as_needed(void)
{
	struct nvmeibt_seg_active	*seg_active;

	NFIN;
	XDLIST_FOREACH_SAFE(seg_active, &(nvmeibt_global_get_global()->seg_active_post_update_action_list)) {
		//nvmeibt_seg_active_mark_zeroing_required_as_needed(seg_active);
		nvmeibt_seg_active_reset_serjio_clean_range_state_on_new_config_or_topo(seg_active);
		// nvmeibt_seg_active_notify_serjio_if_seg_is_being_deleted(seg_active);
	}
	NFOUT;
}

/**
 * Related the data that was probed from the local node hardware (disks + nics)
 * to the configuration data.
 *
 * @author max (3/7/17)
 *
 * @return int
 */
int nvmeibt_topology_relate_hardware_probe_to_config(void)
{
	int									rv = 0;

	NFIN;
	// Relate the news to cur_topo. Might be due to config changes, and not due to local hardware changes
	if (connect_local_disks_with_disks() < 0)
		rv = -1;
	if (connect_disks_with_disk_segments() < 0)
		rv = -1;
	connect_local_disk_segments_with_seg_active();
	nvmeibt_topology_active_mark_reserialization_required();
	mark_all_required_cleanups_for_all_local_segs_on_config_changes_as_needed();
	NFOUT;
	return rv;
}

static int check_local_segments_validity(void)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_disk			*disk;
	int		rv = 0;
	int		j, k;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &nvmeibt_global_get_global()->local_disks_hash) {
		unsigned long long	disk_size_4kblk;

		if (!nvmeibt_local_disk_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(local_disk)) {
			N_Tf(hu299sn, "local_disk=@STR didn't revive its segs yet, skipping", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		disk = NNVMEIBT_LOCAL_DISK_GET_DISK(check_local_segments_validity, local_disk);
		if (!disk) {
			continue;
		}
		disk_size_4kblk = (disk->its_local_disk->from_config.n_pblk * nvmeibt_local_disk_pblk_size(disk->its_local_disk) / 4096);

		// Double loop for detecting overlaps
		for (j = disk->n_segments - 1 - 1; j >= 0; --j) {
			struct nvmeibt_disk_segment			*seg0 = disk->disk_segments[j];
			struct nvmeibt_seg_mgmt				*m0 = &(seg0->seg_mgmt);

			if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(seg0)) {
				N_Tf(uu88bb2 , "seg=@UUID_8 is old, skipping", nvmeibt_seg_UUID_8(seg0));
				continue;
			}

			for (k = disk->n_segments - 1; k > j; --k) {
				struct nvmeibt_disk_segment			*seg1 = disk->disk_segments[k];
				struct nvmeibt_seg_mgmt				*m1 = &(seg1->seg_mgmt);
				int									is_overlap = 0;

				if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(seg1))
					continue;

				is_overlap |= nvmeibt_do_ranges_overlap(m0->lb_s, m0->lb_e, m1->lb_s, m1->lb_e);
				if (is_overlap) {
					bool	is_0_x_done = nvmeibt_disk_segment_is_x_done(nvmeibt_seg_active_get_active_seg_topo(nvmeibt_disk_segment_get_seg_active(seg0)));
					bool	is_1_x_done = nvmeibt_disk_segment_is_x_done(nvmeibt_seg_active_get_active_seg_topo(nvmeibt_disk_segment_get_seg_active(seg1)));
					if (is_0_x_done || is_1_x_done) {
						N_Tf(tcvajwi, "Overlap ignored seg0=@UUID_8 is_x_done=@BOOL lb_s=@LLX lb_e=@LLX, seg1=@UUID_8 is_x_done=@BOOL lb_s=@LLX lb_e=@LLX",
							 nvmeibt_seg_UUID_8(seg0), is_0_x_done, m0->lb_s, m0->lb_e, nvmeibt_seg_UUID_8(seg1), is_1_x_done, m1->lb_s, m1->lb_e);
					} else {
						N_Ef(dgy6746, "Overlapping! seg0=@UUID_8 lb_s=@LLX lb_e=@LLX, seg1=@UUID_8 lb_s=@LLX lb_e=@LLX",
							 nvmeibt_seg_UUID_8(seg0), m0->lb_s, m0->lb_e, nvmeibt_seg_UUID_8(seg1), m1->lb_s, m1->lb_e);
						// Mark conf corrupted only the segment(s) that was modified in last config - it's either one
						// of the 2 or both, cannot be none since in the past either both segments weren't in config
						// or were fine.
						if (seg0->seg_follower.is_modified_in_last_config) {
							nvmeibt_disk_segment_mark_conf_corrupted(seg0);
						}
						if (seg1->seg_follower.is_modified_in_last_config) {
							nvmeibt_disk_segment_mark_conf_corrupted(seg1);
						}
					}
				}
			}
			if (m0->lb_e >= disk_size_4kblk) {
				N_Ef(lo9tir6, "seg1=@UUID_8 lb_e=@LLX size=@LLX", nvmeibt_seg_UUID_8(seg0), m0->lb_e, disk_size_4kblk);
				nvmeibt_disk_segment_mark_conf_corrupted(seg0);
			}
		}
	}
	NFOUT;
	rv = 0;
	return rv;
}

void reset_all_conf_corrupted_flags(void) {
	struct nvmeibt_disk_segment	*disk_segment;
	struct nvmeibt_praid		*praid;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;
	nvmeibt_clear_conf_corrupted();
	XHASHTABLE_FOR_EACH_SAFE(disk_segment, &cur_topo->disk_segments_hash) {
		disk_segment->is_conf_corrupted = 0;
	}
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		praid->praid_mgmt.is_conf_corrupted = 0;
	}
	NFOUT;
}

int nvmeibt_topology_setup_relationships(void)
{
	int		rv = -1;

	NFIN;
	if (connect_disk_with_node_by_config() < 0)
		goto skip;
	if (connect_nics_with_nodes() < 0)
		goto skip;
	if (nvmeibt_topology_relate_hardware_probe_to_config() < 0)
		goto skip;
	if (nvmeibt_praid_validate_praids_config() < 0)
		goto skip;
	if (check_local_segments_validity() < 0)
		goto skip;
	if (trim_stale_gpt_entries_from_local_disks_gpt() < 0)
		goto skip;
#if 1
	if (add_segments_to_local_disks_gpt() < 0)
		goto skip;
#endif

	rv = 0;
skip:
	if (rv < 0) {
		N_Ef(5cga92k, "rv=@INT", rv);
		nvmeibt_abort(ES_FATAL);
	}

	NFOUT;
	return rv;
}

/*******************           Persistency & serialization          ********************/

void nvmeibt_topology_leader_mark_all_modified_praids_report_to_mgmt_due_to_committed_by_majority(void)
{
	struct nvmeibt_praid			*praid;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();

	NFIN;

	TODO(Consider avoiding full report to MGMT upon new leader);
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid)) {
			N_Tf(u876nss, "Skipping praid=@UUID_LE outdated", nvmeibt_praid_UUID(praid));
		}
		else {
			if (	((praid->praid_leader.to_report_praid_lot.topo_ctx.praid_version_major != praid->praid_leader.baseline_praid_lot.topo_ctx.praid_version_major) ||
					 (praid->praid_leader.to_report_praid_lot.topo_ctx.praid_version_minor != praid->praid_leader.baseline_praid_lot.topo_ctx.praid_version_minor))) {
				nvmeibt_praid_lot_duplicate_content(&praid->praid_leader.to_report_praid_lot, &praid->praid_leader.baseline_praid_lot);
				nvmeibt_praid_mark_immediate_report_to_mgmt_required(praid);
			}
		}
	}

	NFOUT;
}

void nvmeibt_topology_leader_resend_all_praids_report_to_mgmt(void)
{
	struct nvmeibt_praid			*praid;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();

	N_Tf(y77uq21,"Resend all praids report");
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid))
			nvmeibt_praid_mark_immediate_report_to_mgmt_required(praid);
	}
}

int nvmeibt_topology_leader_resend_specific_vol_praids_report_to_mgmt(char *vol_name)
{
	struct nvmeibt_block_device					*vol;
	int											i, j;

	XHASHTABLE_FOR_EACH_SAFE(vol, &nvmeibt_global_get_global()->block_devices_hash) {
		if (strcmp(vol->from_config.client_blkdev_name, vol_name) == 0) {
			N_Tf(y77ubu1,"Resend vol=@STR praids report", vol_name);
			for (i = 0; i < vol->n_chunks; i++) {
				struct nvmeibt_chunk	*chunk = vol->chunks[i];
					for (j = 0; j < chunk->n_praids; j++) {
						struct nvmeibt_praid	*praid = chunk->praids[j];
						if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid))
							nvmeibt_praid_mark_immediate_report_to_mgmt_required(praid);
					}
			}
			return 0;
		}
	}
	N_Tf(yo0ubu1,"vol=@STR not found", vol_name);
	return -1;
}

void nvmeibt_topology_mark_update_csv_of_config_and_topo_required(void)
{
	NFIN;

	nvmeibt_global_get_global()->is_update_csv_of_config_and_topo_required = true;

	NFOUT;
}

unsigned long long nvmeibt_topology_leader_get_next_config_version(void)
{
	return (nvmeibt_raft_get_current_term() << 32) | ((nvmeibt_global_get_global()->leader_config_version + 1) & 0xffffffff);
}

static inline bool omit_praid_in_serialized_topo(struct nvmeibt_praid *praid)
{
	return (!nvmeibt_praid_is_serialized(praid) || NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(nvmeibt_praid_get_blkdev(praid)) || nvmeibt_praid_is_being_deleted(praid));
}

void nvmeibt_topology_leader_serialize_baseline_topo_to_wire(void)
{
	struct nvmeibt_praid							*praid;
	int												praids_num = 0, segs_num = 0;
	unsigned int									topo_len;
	struct nvmeibt_topology							*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_Buf								*dst_wire_topo_buf = &(nvmeibt_raft_get_my_raft()->leader_to_commit_wire_topo);
	struct nvmeibt_topology_serialized_topo_header	serialized_header = {0};
	struct nvmeibt_topology_serialized_topo_header	*dst_wire_header_ptr;
	struct nvmeibt_praid_serialized_topo			*dst_praid_wire_topo_ptr;
	struct nvmeibt_serialized_seg_leader_topo		*dst_praid_segs_wire_topo_ptr;
	struct nvmeibt_Str 								*print_s;
	struct nvmeibt_Buf								*src_praid_segs_wire_topo_buf;

	NFIN;
	// Count how many praids and segments we have in order to allocate a buffer
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		if (omit_praid_in_serialized_topo(praid))
			continue;
		praids_num++;
		segs_num += XDLIST_N_ELEMNTS(&(praid->praid_leader.baseline_praid_lot.all_seg_lot_list));
	}
	topo_len = sizeof(struct nvmeibt_topology_serialized_topo_header) +
			   sizeof(struct nvmeibt_praid_serialized_topo) * praids_num +
			   sizeof(struct nvmeibt_serialized_seg_leader_topo) * segs_num;
	NNVMEIBT_BUF_RESIZE(t29vmqp, dst_wire_topo_buf, topo_len);
	memset(dst_wire_topo_buf->data_buf, 0, dst_wire_topo_buf->buf_len);

	// Now build the topo
	TODO(Most of the following fields are probably redundant with the TLV. At least topo_len and mgmt_config_version);
	memset(&serialized_header, 0, sizeof(serialized_header));
	memcpy(serialized_header.topo_name, nvmeibt_topology_binary_topo_header, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
	serialized_header.sw_ver = TOMA_SW_COMPATIBILITY_VER;
	serialized_header.topo_len = topo_len;
	serialized_header.praids_num = praids_num;
	serialized_header.res_1 = 0;
	serialized_header.res_2 = 0;
	dst_wire_header_ptr = (struct nvmeibt_topology_serialized_topo_header *)(dst_wire_topo_buf->data_buf);
	nvmeibt_topology_convert_header_le_be(&serialized_header, dst_wire_header_ptr);
	dst_praid_wire_topo_ptr = (struct nvmeibt_praid_serialized_topo *)(dst_wire_header_ptr + 1);
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		if (omit_praid_in_serialized_topo(praid))
			continue;
		*dst_praid_wire_topo_ptr = praid->praid_leader.praid_wire_topo;
		dst_praid_segs_wire_topo_ptr = dst_praid_wire_topo_ptr->segs;
		src_praid_segs_wire_topo_buf = &praid->praid_leader.segs_wire_topo_buf;
		memcpy(dst_praid_segs_wire_topo_ptr, src_praid_segs_wire_topo_buf->data_buf, src_praid_segs_wire_topo_buf->buf_len);
		dst_praid_wire_topo_ptr = (struct nvmeibt_praid_serialized_topo *)((char *)dst_praid_segs_wire_topo_ptr + src_praid_segs_wire_topo_buf->buf_len);
	}

	print_s = NNVMEIBT_STR_ALLOC(htyw76a);
	NNVMEIBT_STR_RESIZE_BUF(loazxc6, print_s, 8192);
	nvmeibt_topology_print((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, dst_wire_topo_buf, 1);
	NVMEIBT_LONG_TRACE_WRAPPER(poiurea, "SERIALIZED BASELINE TOPOLOGY", nvmeibt_Str_str(print_s), nvmeibt_Str_strlen(print_s));
	NNVMEIBT_STR_FREE(rpsnhs1, print_s);
	// No need to convert to wire format, we already have a concatenation of praid wire buffers
	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
	NFOUT;
}

int nvmeibt_topology_serialize_active_topology(void)
{
	int												rv = 0;
	int												j;
	struct nvmeibt_topology							*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_Buf								*serialized_and_wire_topo_buf = &(cur_topo->buf_of_follower_wire_topo);	TODO(Starts as serialized, and converted to wire in_place)
	struct nvmeibt_local_disk						*local_disk;
	struct nvmeibt_disk								*disk;
	enum nvmeibt_topology_shutdown_state			shutdown_state;
	struct nvmeibt_disk_segment_topo_ctx			*applied_seg_topo_ctx;
	struct nvmeibt_disk_segment_topo_ctx			*active_seg_topo_ctx;
	struct nvmeibt_seg_active						*seg_active;
	unsigned int									topo_len, n_seg;
	struct nvmeibt_active_topo_header				*header_ptr = NULL;
	struct nvmeibt_serialized_seg_active_topo		*serialized_and_wire_seg_active_ptr = NULL;	TODO(Starts as serialized, and converted to wire in_place)
	struct nvmeibt_Str 								*print_s;

	NFIN;
	if (!nvmeibt_topology_active_is_reserialization_required() || !cur_topo->is_serialize_active_topo_for_leader) {
		goto out;
	}

	if ((shutdown_state = nvmeibt_topology_applied_get_shutdown_state()) == NVMEIBT_SHUTDOWN_FIRST) {
		nvmeibt_topology_applied_mark_shutdown();
	}
	N_Tf(u87u86t, "shutdown_state=@INT", shutdown_state);

	if (cur_topo->in_transmission_rep_cnt) {
		N_Tf(i98nsg5, "Previous transmission not finished, tx_remained=@INT. Skipping for now", cur_topo->in_transmission_rep_cnt);
		goto out;
	}

	N_Tf(o9o0o73, "is_reserialization_required");
	cur_topo->running_local_serialization_version++;
	N_Tf(beud95f, "running_local_serialization_version=@LLU", nvmeibt_global_get_global()->running_local_serialization_version);

	n_seg = 0;
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		disk = NNVMEIBT_LOCAL_DISK_GET_DISK(oiq23mn, local_disk);
		if (disk) {
			n_seg += disk->n_segments;
		} else {
			N_Tf(5sgv2kq, "Skipped serialization for local_disk=@STR its_disk=NULL", nvmeibt_local_disk_display(local_disk));
		}
	}
	topo_len = sizeof(struct nvmeibt_serialized_seg_active_topo) * n_seg + sizeof(struct nvmeibt_active_topo_header);
	NNVMEIBT_BUF_RESIZE(uiop2wn, serialized_and_wire_topo_buf, topo_len);
	header_ptr = (struct nvmeibt_active_topo_header *)(serialized_and_wire_topo_buf->data_buf);
	memcpy(header_ptr->topo_name, nvmeibt_topology_binary_active_topo_header, NVMEIBT_TOPOLOGY_BIN_NAME_LEN);
	header_ptr->sw_ver = TOMA_SW_COMPATIBILITY_VER;
	header_ptr->res = 0;
	serialized_and_wire_seg_active_ptr = (struct nvmeibt_serialized_seg_active_topo *)(header_ptr + 1);
	n_seg = 0;

	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
			continue;
		}
		disk = NNVMEIBT_LOCAL_DISK_GET_DISK(uyi8c04, local_disk);
		if (!disk) {
			continue;
		}
		for (j = 0; j < disk->n_segments; j++) {
			struct nvmeibt_disk_segment	*disk_segment = disk->disk_segments[j];
			seg_active = nvmeibt_disk_segment_get_seg_active(disk_segment);
			if (!seg_active) {
				N_Tf(sk9898g, "Skip @UUID_8 - no seg_active", nvmeibt_seg_UUID_8(disk_segment));
				continue;
			}
			active_seg_topo_ctx = nvmeibt_seg_active_get_active_seg_topo(seg_active);

			if (!nvmeibt_disk_segment_is_x_done(active_seg_topo_ctx)) {
				if (!nvmeibt_disk_segment_is_config_OK(disk_segment) && !nvmeibt_seg_lot_is_deleted_in_config(&disk_segment->seg_follower.applied_seg_lot)) {
					N_Tf(mjutirk, "Skip @UUID_8 is_config_OK=@BOOL seg_lot_is_deleted_in_config=@BOOL", nvmeibt_seg_UUID_8(disk_segment),
						 nvmeibt_disk_segment_is_config_OK(disk_segment), nvmeibt_seg_lot_is_deleted_in_config(&disk_segment->seg_follower.applied_seg_lot));
					continue;
				}
			}
			nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd(seg_active);	TODO(probably already updated, and not needed here);
			if (shutdown_state == NVMEIBT_SHUTDOWN_FIRST)
				active_seg_topo_ctx->active_seg_ser_ver++;

			// Always serialize all the local segs, otherwise the leader will consider them dead
			if (shutdown_state == NVMEIBT_SHUTDOWN_NONE) {
				applied_seg_topo_ctx = &disk_segment->seg_follower.applied_seg_lot.seg_topo;
				nvmeibt_strlcpy(serialized_and_wire_seg_active_ptr->eyecatcher, "SFW",
								sizeof(serialized_and_wire_seg_active_ptr->eyecatcher));
				serialized_and_wire_seg_active_ptr->uuid = *nvmeibt_seg_UUID(disk_segment);
				serialized_and_wire_seg_active_ptr->dirty_bits_state = active_seg_topo_ctx->dirty_bits_state;
				serialized_and_wire_seg_active_ptr->active_praid_version_major = applied_seg_topo_ctx->seg_praid_version_major;
				serialized_and_wire_seg_active_ptr->active_praid_version_minor = applied_seg_topo_ctx->seg_praid_version_minor;
				serialized_and_wire_seg_active_ptr->active_seg_flags = active_seg_topo_ctx->active_seg_flags;
				serialized_and_wire_seg_active_ptr->active_seg_flags.is_drive_write_error |= (disk_segment->is_drive_write_error | disk->is_drive_write_error);
				serialized_and_wire_seg_active_ptr->dirty_bits_init_mode = active_seg_topo_ctx->dirty_bits_init_mode;
				serialized_and_wire_seg_active_ptr->stale_locks_init_mode = active_seg_topo_ctx->stale_locks_init_mode;
				serialized_and_wire_seg_active_ptr->res_1 = 0;
				serialized_and_wire_seg_active_ptr->res_2 = 0;
				serialized_and_wire_seg_active_ptr->res_3 = 0;
				serialized_and_wire_seg_active_ptr->active_seg_ser_ver = 0; // for comparation

				if (memcmp(serialized_and_wire_seg_active_ptr, &seg_active->prev_serialized_topo, sizeof(*serialized_and_wire_seg_active_ptr))) {
					seg_active->prev_serialized_topo = *serialized_and_wire_seg_active_ptr;
					active_seg_topo_ctx->active_seg_ser_ver++;
				}
				// Report a client problem only once, leader remembers our report if needed
				active_seg_topo_ctx->active_seg_flags.did_any_client_report_about_problems = 0;
				serialized_and_wire_seg_active_ptr->active_seg_ser_ver = active_seg_topo_ctx->active_seg_ser_ver;
				n_seg++;
				serialized_and_wire_seg_active_ptr++;
			}
		}
	}
	nvmeibt_topology_active_clear_reserialization_required();	// Might be redundant

	// RESIZE() After recalc of the actual n_seg
	topo_len = sizeof(struct nvmeibt_serialized_seg_active_topo) * n_seg + sizeof(struct nvmeibt_active_topo_header);
	NNVMEIBT_BUF_RESIZE(nu65fq9, serialized_and_wire_topo_buf, topo_len);
	header_ptr->topo_len = topo_len;
	header_ptr->segs_num = n_seg;
	//
	print_s = NNVMEIBT_STR_ALLOC(ytuqo9c);
	NNVMEIBT_STR_RESIZE_BUF(loi2wsb, print_s, 8192);
	nvmeibt_topology_follower_print((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, serialized_and_wire_topo_buf->data_buf);
	NVMEIBT_LONG_TRACE_WRAPPER(bhui345, "SERIALIZED ACTIVE TOPOLOGY", nvmeibt_Str_str(print_s), nvmeibt_Str_strlen(print_s));
	NNVMEIBT_STR_FREE(fy67tu9, print_s);
	// Perform LE/BE convert in_place
	serialized_and_wire_seg_active_ptr = (struct nvmeibt_serialized_seg_active_topo *)(header_ptr + 1);
	for (j = 0; j < header_ptr->segs_num; j++) {
		nvmeibt_disk_segment_convert_active_bin_topo_le_be(serialized_and_wire_seg_active_ptr);
		serialized_and_wire_seg_active_ptr++;
	}
	nvmeibt_topology_convert_follower_header_le_be(header_ptr);
	// Slightly inefficient. First serialize into the topo_buf, and then copy it into the follower_to_leader_wire_buf
	//  Memory wise, I could first allocate the follower_to_leader_wire_buf and serialize the topo in there
	// Following the Leader's concept that the buffer is an assembly of several buffers that are serialized beforehand
	NNVMEIBT_TOMA_FREE(ebwik25, nvmeibt_raft_get_my_raft()->follower_to_leader_wire_buf);
	nvmeibt_raft_get_my_raft()->follower_to_leader_wire_buf = nvmeibt_raft_generate_persist_and_wire_buf(
		nvmeibt_raft_get_current_term(),	// Not really important
		RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_committed),	// Not really important
		nvmeibt_kafka_get_kafka_mgmt_zone_number(),    // Not really important
		&(nvmeibt_raft_get_my_raft()->voted_for_uuid),	// Not really important
		nvmeibt_global_get_mgmt_DB_uuid(),	// Not really important
		nvmeibt_raft_get_persistent_leader_append_entries_time_ns(),	// The follower can only echo back its persistent value
		nvmeibt_raft_get_persistent_leader_topo_calc_time_ns(),	// The follower can only echo back its persistent value
		nvmeibt_raft_get_guaranteed_sw_ver(),
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed), -1, serialized_and_wire_topo_buf->data_buf, serialized_and_wire_topo_buf->buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed), -1, NULL, 0,
		RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed), -1, NULL, 0,
		RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed), RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed), NULL, 0);
out:
	NFOUT;
	return rv;
}

int nvmeibt_topology_parse_committed_topology(void) {
	int			rv = 0;
	char		*topo_buf;
	int			topo_buf_len;

	NFIN;
	topo_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full, TLV_TYPE_TOPO_FULL, &topo_buf);
	if (topo_buf_len == 0) {
		N_Tf(5vsjhs8, "Empty topo. Ignoring.");
	} else if (topo_buf_len < (int)sizeof(struct nvmeibt_topology_serialized_topo_header)) {
		N_Ef(siunneo, "Corrupted topo len=@INT", topo_buf_len);
		rv = -1;
	} else {
		rv = nvmeibt_parse_buf(topo_buf, topo_buf_len, 0, NVMEIBT_NOT_INITIALIZED_SER_VER, NULL, NVMEIBT_CSV_TYPE_TOPO, NULL);
	}
	NFOUT;
	return rv;
}

int nvmeibt_topology_parse_a_config(enum NVMEIBT_CSV_TYPE content_type, struct nvmeibt_Str *JSON_output)
{
	int			rv = -1;
	char		*config_buf;
	int			config_buf_len;

	NFIN;
	reset_all_conf_corrupted_flags();
	config_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(
		nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full, (content_type == NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES ? TLV_TYPE_TOPO_CONFIG_FULL : TLV_TYPE_KAFKA_MGMT_CONFIG_FULL), &config_buf);
	if (config_buf_len == 0) {
		N_Tf(jajasd3, "volumes config is empty");
		rv = 0;
		goto out;
	}
	if (nvmeibt_parse_buf(config_buf, config_buf_len, 0, NVMEIBT_NOT_INITIALIZED_SER_VER, NULL, content_type, JSON_output) < 0) {
		N_Ef(aass324, "failed to parse management configuration");
		goto out;
	}

	/* dumper: log this change-of-configuration event */
	nvmeibt_dumper_event_mgmt_config();

	if (nvmeibt_topology_setup_relationships() < 0)
		goto out;

	/* Post config update actions */
	TODO(Separate vol from target from H/W config);
	nvmeibt_seg_active_stop_all_recoveries_and_registrations_on_deleted_segs();

	rv = 0;
out:
	NFOUT;
	return rv;
}

static void update_applied_topology(void)
{
	struct nvmeibt_praid				*praid;
	struct nvmeibt_praid_follower		*praid_follower;
	struct nvmeibt_local_disk			*local_disk;
	struct nvmeibt_disk					*disk;
	int									seg_idx;
	struct nvmeibt_disk_segment			*seg;
	struct nvmeibt_seg_active			*seg_active;
	struct nvmeibt_praid_topo_ctx		*committed_praid_topo;
	bool								is_praid_config_ver_changed;
	bool								is_praid_during_cold_recovery;

	NFIN;
	nvmeibt_global_get_global()->last_apply_time = nvmeibt_global_get_cur_event_start_time();
	XHASHTABLE_FOR_EACH_SAFE(praid, &nvmeibt_global_get_global()->praids_hash) {
		if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid)) {
			N_Tf(gegey33, "Skipping praid=@UUID_LE outdated", nvmeibt_praid_UUID(praid));
			continue;
		}
		praid_follower = &praid->praid_follower;
		committed_praid_topo = &praid_follower->committed_praid_lot.topo_ctx;
		is_praid_config_ver_changed = (praid_follower->applied_praid_lot.from_config.version != praid_follower->committed_praid_lot.from_config.version);
		// The duplication must be performed before processing the praid because some segs can be already deleted !
		nvmeibt_praid_lot_duplicate_content(&praid_follower->applied_praid_lot, &praid_follower->committed_praid_lot);
		is_praid_during_cold_recovery = nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(praid_follower->applied_praid_lot.topo_ctx.registrants_sync_cmd);
		praid_follower->applied_io_perms = nvmeibt_praid_calc_io_perms(praid, is_praid_during_cold_recovery);

		if (is_praid_config_ver_changed)
			nvmeibt_praid_mark_all_praid_segs_post_update_actions_required(praid);

		praid->praid_leader.is_waiting_for_timeout_since_activation_attempt &= !committed_praid_topo->is_activated;
	}

	//update active topo for all local segs
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &nvmeibt_global_get_global()->local_disks_hash) {
		disk = NNVMEIBT_LOCAL_DISK_GET_DISK(agt64y3, local_disk);
		if (!disk) {
			N_Tf(ju88987, "Missing disk for local_disk=@STR", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		for (seg_idx = 0; seg_idx < disk->n_segments; seg_idx++) {
			seg = disk->disk_segments[seg_idx];
			seg_active = nvmeibt_disk_segment_get_seg_active(seg);
			if (seg_active) {
				if (!nvmeibt_seg_lot_is_config_OK(nvmeibt_seg_active_get_applied_seg_lot(seg_active)))
					nvmeibt_seg_active_stop_all_recoveries_and_registrations(seg_active, 1);
				else
					nvmeibt_seg_active_upd_active_topo_from_applied_topo(seg_active);
				disk->is_drive_write_error |= nvmeibt_seg_active_get_applied_seg_lot(seg_active)->seg_topo.active_seg_flags.is_drive_write_error;
				seg->is_drive_write_error |= nvmeibt_seg_active_get_applied_seg_lot(seg_active)->seg_topo.active_seg_flags.is_drive_write_error;
			}
		}
	}
	NFOUT;
}

void nvmeibt_topology_apply_the_latest_committed_config_and_topo(void)
{
	NFIN;
	// We do not support applying a new (old) topology that is not the applied_LOG_index
	//	my_raft->applied_LOG_index = min(msg->applied_LOG_index, COMMITTED_LAST_LOG_INDEX(my_raft));    // From protocol
	if (nvmeibt_raft_is_shutdown_triggered()) {
		// Ignore topology_changes
		goto out;
	}
	update_applied_topology();
	nvmeibt_seg_active_generate_all_segs_topo_for_clients();
	nvmeibt_global_call_all_seg_active_post_update_actions();
	SET_RAFT_COMMIT_LIFECYCLE_VAL(b57xage, TOPO,        follower_applied, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,        follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(msyuehz, TOPO_CONFIG, follower_applied, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
	nvmeibt_register_open_all_eligible_seg_actives_for_use();
out:
	NFOUT;
}

static void leader_remove_node_disks_whose_segs_are_not_in_remote_applied(struct nvmeibt_raft_member *remote_member)
{
	int 									n_remote_disk_segs_missing, n_disk_segs_x_done, n_disk_segs_first_use_ever;
	int										i, j;
	struct nvmeibt_disk						*disk;
	struct nvmeibt_disk_segment				*disk_segment;
	struct nvmeibt_seg_leader				*seg_leader;
	struct nvmeibt_praid					*praid;

	NFIN;
	// Remove missing disks from the remote node
	// Go over the node's disks' segments and decide whether the disk is still on
	//  the node based on whether the segments were reported in local_serialization_version.
	// _Wf() if some of a disk's segments are reported, and some are not
	for (i = remote_member->n_disks_leader - 1; i >= 0; --i) {
		disk = remote_member->disks_leader[i];
		n_remote_disk_segs_missing = 0;
		n_disk_segs_x_done = 0;
		n_disk_segs_first_use_ever = 0;
		for (j = disk->n_segments - 1; j >= 0; --j) {
			disk_segment = disk->disk_segments[j];
			seg_leader = &disk_segment->seg_leader;
			praid = disk_segment->seg_mgmt.its_praid;

			// If a leader has never calculated this disk segment's state, it may have arrived in a configuration,
			// but may not be sent back yet in the applied
			if (!nvmeibt_disk_segment_is_dirty_bits_state_calculated(seg_leader->baseline_seg_lot.seg_topo.dirty_bits_state))
				continue;

			if (seg_leader->last_remote_applied_node_local_serialization_version == remote_member->last_local_serialization_version) {
				seg_leader->is_removed_from_remote_applied = 0;
				// Was just reported by the node's disk
				if (n_remote_disk_segs_missing > 0) {
					N_Wf(twkso2x, "seg=@UUID_8 is alive on disk=@UUID_LE that has unreported segments",
						 nvmeibt_seg_UUID_8(disk_segment), nvmeibt_disk_UUID(disk));
/*
					N_Wf(t_11_toma_nirap, "vol=@VOL praid=@PRAID disk_segment=@UUID_8 "
						 "last_remote_applied_node_local_serialization_version=@INT last_local_serialization_version=@INT",
						 nvmeibt_praid_get_blkdev_name(praid),
						 nvmeibt_praid_UUID(praid),
						 nvmeibt_seg_UUID_8(disk_segment),
						 seg_leader->last_remote_applied_node_local_serialization_version,
						 remote_node->raft_ctx.last_local_serialization_version);
*/
				}
			}
			else {
/*
				N_Tf(t_02_toma_nirap, "vol=@VOL praid=@PRAID disk_segment=@UUID_8 "
					"last_remote_applied_node_local_serialization_version=@INT last_local_serialization_version=@INT",
					nvmeibt_praid_get_blkdev_name(praid),
					nvmeibt_praid_UUID(praid),
					nvmeibt_seg_UUID_8(disk_segment),
					seg_leader->last_remote_applied_node_local_serialization_version,
					remote_node->raft_ctx.last_local_serialization_version);
*/
				// We didn't get it in this report
				nvmeibt_seg_remote_reset(&(disk_segment->seg_leader.remote_seg_topo), disk_segment);
				NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(cjcf55q, praid);
				seg_leader->is_removed_from_remote_applied = 1;
				if (	(!nvmeibt_disk_segment_is_x_done(&seg_leader->baseline_seg_lot.seg_topo) &&
						 !nvmeibt_disk_segment_is_mem_tbl_init_FIRST_USE_EVER(&seg_leader->baseline_seg_lot.seg_topo))) {
					N_Tf(t_03_toma_nirap, "seg=@UUID_8 is not X_DONE nor FIRST_USE_EVER and missing from ldisk=@STR",
						 nvmeibt_seg_UUID_8(disk_segment), nvmeibt_disk_get_ldisk_id_str(disk));
/*
					N_Wf(t_13_toma_nirap, "vol=@VOL praid=@PRAID disk_segment=@UUID_8 "
						 "last_remote_applied_node_local_serialization_version=@INT last_local_serialization_version=@INT",
						 nvmeibt_praid_get_blkdev_name(praid),
						 nvmeibt_praid_UUID(praid),
						 nvmeibt_seg_UUID_8(disk_segment),
						 seg_leader->last_remote_applied_node_local_serialization_version,
						 remote_node->raft_ctx.last_local_serialization_version);
*/
					n_remote_disk_segs_missing++;
				}
				else {
					if (nvmeibt_disk_segment_is_x_done(&seg_leader->baseline_seg_lot.seg_topo)) {
						n_disk_segs_x_done++;
					} else {
						n_disk_segs_first_use_ever++;
					}
				}
			}
		}
		if ((n_remote_disk_segs_missing + n_disk_segs_x_done) == disk->n_segments) {
			nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(disk);
		} else if (n_remote_disk_segs_missing && ((n_remote_disk_segs_missing + n_disk_segs_x_done + n_disk_segs_first_use_ever) != disk->n_segments)) {
			N_Wf(itit584, "ldisk=@STR some segs are missing, n_:(seg=@INT missing=@INT x_done=@INT first_use_ever=@INT)",
				 nvmeibt_disk_get_ldisk_id_str(disk), disk->n_segments, n_remote_disk_segs_missing, n_disk_segs_x_done, n_disk_segs_first_use_ever);
		}
	}
	NFOUT;
}

int nvmeibt_topology_leader_new_remote_applied_topology_arrived(const void *remote_topology_data,
																int remote_topology_data_len, struct nvmeibt_raft_member *remote_member)
{
	int							rv = 0;

	NFIN;
	if ((size_t)remote_topology_data_len < sizeof(struct nvmeibt_active_topo_header)) {
		N_Ef(ee66ba9, "Corrupted topo len=@INT", remote_topology_data_len);
		rv = -1;
		goto out;
	}

	rv = nvmeibt_parse_buf(remote_topology_data, remote_topology_data_len, 1, remote_member->last_local_serialization_version, remote_member, NVMEIBT_CSV_TYPE_REMOTE_APPLIED, NULL);
	if (rv == 0)
		leader_remove_node_disks_whose_segs_are_not_in_remote_applied(remote_member);
out:
	NFOUT;
	return rv;
}

void nvmeibt_topology_reset_due_to_convert_to_leader(void)
{
	struct nvmeibt_praid		*praid;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;
#if  0 // always take from the baseline, no need to reset. maybe take committed to baseline here (if not done by the prev func
	XHASHTABLE_FOR_EACH_SAFE(disk_segment, &cur_topo->disk_segments_hash) {
		nvmeibt_seg_lot_reset_topo_ctx(&disk_segment->seg_leader.calculated_seg_lot);
	}
#endif
	nvmeibt_global_get_global()->leader_config_version = 0;
	// Initialize the leader_to_commit value based on the baseline, which is the follower_committed
	SET_RAFT_COMMIT_LIFECYCLE_VAL(gjdrtyd, TOPO, leader_committed_by_majority, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(tbdsjkn, TOPO_CONFIG, leader_committed_by_majority, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(43vsdf8, KAFKA_MGMT_CONFIG, leader_committed_by_majority, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(5nf9dlu, RAFT_MEMBERS, leader_committed_by_majority, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(bch5km7, RAFT_MEMBERS_SEQ_NO, leader_committed_by_majority, nvmeibt_offset_and_idx_uninitialized);
	//
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vy58uj2, TOPO, leader_to_commit, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vy58ujr, TOPO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(bmdk5k2, TOPO_CONFIG, leader_to_commit, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(bmdk5ko, TOPO_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(pfmwk32, KAFKA_MGMT_CONFIG, leader_to_commit, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(pfmwk3k, KAFKA_MGMT_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(5xbwk22, RAFT_MEMBERS, leader_to_commit, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(5xbwk2m, RAFT_MEMBERS, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(9js4wy2, RAFT_MEMBERS_SEQ_NO, leader_to_commit, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(9js4wyi, RAFT_MEMBERS_SEQ_NO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
	//
/* Let the comparison (before send) of calculated and leader_to_commit handle this
	SET_RAFT_COMMIT_LIFECYCLE_VAL(iwopxjb, TOPO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(1zoxreb, TOPO_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(csfghwe, KAFKA_MGMT_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(dh8ch5e, RAFT_MEMBERS, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(dh8ch5e, RAFT_MEMBERS_SEQ_NO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
*/
	//
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cvsh2i2, TOPO, leader_calculated, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cvsh2i4, TOPO, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cbajhb2, TOPO_CONFIG, leader_calculated, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cbajhbr, TOPO_CONFIG, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(b95kdl2, KAFKA_MGMT_CONFIG, leader_calculated, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(b95kdl4, KAFKA_MGMT_CONFIG, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbnxdke, RAFT_MEMBERS, leader_calculated, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbnxu86, RAFT_MEMBERS, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbnxau7, RAFT_MEMBERS_SEQ_NO, leader_calculated, nvmeibt_offset_and_idx_uninitialized);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbnxmr5, RAFT_MEMBERS_SEQ_NO, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
	//
	XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
		if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid))
			N_Tf(imfjj2, "Skipping praid=@UUID_LE outdated", nvmeibt_praid_UUID(praid));
		else
			nvmeibt_praid_reset_due_to_convert_to_leader(praid);
	}
	if (!nvmeibt_toma_is_running_as_a_utility()) {
		nvmeibt_raft_reset_leader_calculated_IIRs();	// We need to serialize those values that were read from JSON
	}
	NFOUT;
}

void nvmeibt_topology_serialize_conf_and_topo_if_needed(void)
{
	NFIN;
	// The baseline config & topo, contain only pRAIDs, while the mgmt config also contains the blkdev(s)
	if (RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_calculated) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit)) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(7ceuak2, TOPO_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_calculated));
		nvmeibt_mm_json_leader_serialize_baseline_topo_config_to_wire(-1LL);
	}
	if (RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_calculated) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit)) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(v6gsjkw, TOPO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_calculated));
		nvmeibt_topology_leader_serialize_baseline_topo_to_wire();
	}
	if (	(RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_calculated) != RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_to_commit) ||
			 RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_calculated) != RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_to_commit))) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(cvsaiqw, RAFT_MEMBERS, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_calculated));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(tcvshjj, RAFT_MEMBERS_SEQ_NO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_calculated));
		nvmeibt_raft_leader_generate_leader_to_commit_wire_raft_members_buf();
	}
	if (RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated) != RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_to_commit)) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(eisms7v, KAFKA_MGMT_CONFIG, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated));
		nvmeibt_mm_json_leader_serialize_kafka_mgmt_config_to_wire();
	}
	NFOUT;
}

static void mark_recalc_required_for_all_praids_if_config_became_not_corrupted(void)
{
	struct nvmeibt_praid			*praid;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();

	if (nvmeibt_conf_became_not_corrupted()) {
		XHASHTABLE_FOR_EACH_SAFE(praid, &cur_topo->praids_hash) {
			NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(dyye226, praid);
		}
	}
}

void nvmeibt_topology_calc_topology(void)
{
	struct nvmeibt_praid			*praid;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_praid_leader		*praid_leader;
	bool							is_conf_changed, is_topo_changed, is_conf_change_requires_new_topo;
	struct timespec					now;
	int64_t							time_diff_nsec;
	struct timespec					start_timespec;

	NFIN;
	if (nvmeibt_raft_is_shutdown_triggered()) {
		N_Tf(nju853c, "Not calculating new topologies after triggering shutdown");
		goto out;
	}
	if (nvmeibt_topology_freeze_topo) {
		N_Tf(nj2m93c, "Topo is freezed");
		goto out;
	}
	mark_recalc_required_for_all_praids_if_config_became_not_corrupted();
	if (!nvmeibt_topology_leader_is_recalc_required()) {
		goto out;
	}

	N_Tf(ufy123c, "Recalc topology");
	getnstimeofday(&start_timespec);
	ZEROINIT(nvmeibt_global_adaptive_timeouts()->n_praids);
	nvmeibt_topology_leader_clear_recalc_required();
	for (int i = XDLIST_N_ELEMNTS(&cur_topo->praid_topo_recalc_list); i > 0; i--) {
		praid = XDLIST_FIRST(&cur_topo->praid_topo_recalc_list);
		if (praid == NULL)
			break;
		getnstimeofday(&now);
		time_diff_nsec = timespec_diff_ns(now, nvmeibt_global_get_cur_event_start_time());
		N_Tf(xnnkw33, "time from event start=@INT64 nsec", time_diff_nsec);
		if (time_diff_nsec * 2 > nvmeibt_raft_get_effective_heartbeat_timeout_ns()) {
			nvmeibt_topology_leader_mark_recalc_required();
			N_Tf(xnnkw44, "praids topo calc time exceeded, continue on the next heartbeat");
			break;
		}

		NVMEIBT_PRAID_CLEAR_TOPO_RECALC_REQUIRED(ju9q299, praid);
		if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(praid)) {
			N_Tf(itu86ut, "Skipping praid=@UUID_LE outdated", nvmeibt_praid_UUID(praid));
			continue;
		}
		praid_leader = &praid->praid_leader;
		nvmeibt_praid_lot_duplicate_content(&praid_leader->calculated_praid_lot, &praid_leader->baseline_praid_lot);
		is_conf_change_requires_new_topo = nvmeibt_praid_upd_calculated_lot_from_praid_mgmt(praid);
		nvmeibt_praid_leader_calc_topo_main(praid);

		if (!praid_leader->calculated_praid_lot.topo_ctx.is_activated &&
			!praid_leader->baseline_praid_lot.topo_ctx.is_activated) {
			N_Tf(yueet22, "Skipping praid=@UUID_LE remained not activated", nvmeibt_praid_UUID(praid));
			continue;
		}
		is_conf_changed = (praid_leader->calculated_praid_lot.from_config.version >
						   praid_leader->baseline_praid_lot.from_config.version);
		is_topo_changed = (praid_topo_to_last_log_index(&(praid_leader->calculated_praid_lot.topo_ctx)) >
						   praid_topo_to_last_log_index(&(praid_leader->baseline_praid_lot.topo_ctx)));
		if (is_conf_change_requires_new_topo && !is_topo_changed) {
			N_Tf(yueet89, "Skipping praid=@UUID_LE conf cannot be changed without topo change", nvmeibt_praid_UUID(praid));
			continue;
		}

		if (is_conf_changed || is_topo_changed) {
			// The calculation yielded a new valid topology
			nvmeibt_praid_leader_we_have_a_new_baseline(praid, &praid_leader->calculated_praid_lot);
		}
	}
	nvmeibt_topology_serialize_conf_and_topo_if_needed();

	/* dumper: log this new global topology */
	nvmeibt_dumper_event_global_topo();
	getnstimeofday(&now);
	time_diff_nsec = timespec_diff_ns(now, start_timespec);
	nvmeib_iir_add_sample(&(nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns_IIR), time_diff_nsec);
	// NVMEIB_IIR_DUMP(usnfzi4, nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns_IIR, time_diff_nsec);

out:
	NFOUT;
}


static void reregister_peer_nics(void)
{
	struct nvmeibt_nic		*nic;

	NFIN;

	XHASHTABLE_FOR_EACH_SAFE(nic, &nvmeibt_global_get_global()->nics_hash) {
		if (nic->transport != rtr_unknown) {
			nvmeibt_nm_add_remote_nic(nvmeibt_get_nw_node() ,nic);
		}
	}
	NFOUT;
}

static void disk_change_event_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	NFIN;

	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void disk_change_event_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct disk_event_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct disk_event_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_topology_disk_change_event_freer, entry);

	NFOUT;
}

static void disk_change_event_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct disk_event_wq_entry *entry;
	struct nvmeibt_local_disk *local_disk = NULL;
	struct nvmeibt_ascii_uuid	ldisk_id;

	NFIN;

	entry = container_of(wq_entry, struct disk_event_wq_entry, wq_entry);
	nvmeibt_strlcpy(ldisk_id.str, entry->disk_change_msg.disk_id, sizeof(ldisk_id.str));

	/* if wq_entry was canceled, then set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		// nothing in this case?
		// OL: WRONG, FIX THIS?
	}

	// We need to raise the local_disk version, since we know that all those who were executing on
	// the old active version are done. We raise the active version now so that all new executions
	// start with the next active version.
	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, &(nvmeibt_global_get_global()->local_disks_hash));
	if (local_disk) {
		// If the disk is to be added by this hardware event, then the versioning is taken care of by local_disk_add.
		// This part only takes care of some hardware event which happens on an existing disk.

		local_disk->CHANGE_EVENT_counters.active_zeroing_CHANGE_no = local_disk->CHANGE_EVENT_counters.last_CHANGE_no;
	}

	N_Tf(nju8887, "Handling DISK_CHANGE_EVENT on " LOCAL_DISK_LOG_FMT " op=@OP_CHR",
		 entry->disk_change_msg.disk_id, entry->disk_change_msg.native_serial_str, entry->disk_change_msg.nsid, entry->disk_change_msg.op);

	if (entry->disk_change_msg.op == 'a' || entry->disk_change_msg.op == 'c') {
		enum nvmeibt_add_rv rv;
		char config_str[NVMEIBT_MAX_CSV_LINE_LENGTH];
		int n_written =
			snprintf(config_str, sizeof(config_str), "%s,%llu,%llu,%u,%u,%u,%u,%s,%u,%s,%llu,%s,%s", entry->disk_change_msg.disk_id, entry->disk_change_msg.n_blocks, entry->disk_change_msg.n_hw_blocks, entry->disk_change_msg.block_size,
					 entry->disk_change_msg.max_request_size, entry->disk_change_msg.seq, entry->disk_change_msg.nsid, entry->disk_change_msg.dev_name,
					 entry->disk_change_msg.metadata, entry->disk_change_msg.status, entry->disk_change_msg.vendor_id, entry->disk_change_msg.model_str,
					 entry->disk_change_msg.native_serial_str);
		NTOMA_ASSERT(trace_03_topology_disk_change_event_finalize, n_written < (int)sizeof(config_str), "incorrect usage of csv, long lines?");

		N_Tf(aast656, "Add with config str @STR", config_str);
		rv = nvmeibt_local_disk_add_from_config(config_str, 0);
		if (rv ==  NVMEIBT_ADD_FAILED || rv ==  NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL) {
			N_Ef(4bah5n2, "Error handling DISK_CHANGE_EVENT " LOCAL_DISK_LOG_FMT " op=@OP_CHR rv=@RV",
				 entry->disk_change_msg.disk_id, entry->disk_change_msg.native_serial_str, entry->disk_change_msg.nsid, entry->disk_change_msg.op, rv);
			if (rv ==  NVMEIBT_ADD_FAILED) {
				N_Ef(error_01_topology_disk_change_event_finalize, "@STR", config_str);
				nvmeibt_abort(ES_FATAL);
			}
			goto out;
		}
	}
	else if (entry->disk_change_msg.op == 'r') {
		// Disk removed
		nvmeibt_local_disk_remove_by_ldisk_id_str(ldisk_id.str);
	}
	else if (entry->disk_change_msg.op == 's') {
		nvmeibt_local_disk_update_serjio_state(
				entry->disk_change_msg.disk_id,
				entry->disk_change_msg.native_serial_str,
				entry->disk_change_msg.nsid,
				entry->disk_change_msg.metadata);
	}
	else {
		N_Ef(mkmki98, "Unsupported DISK_CHANGE_EVENT " LOCAL_DISK_LOG_FMT " op=@OP_CHR",
			 entry->disk_change_msg.disk_id, entry->disk_change_msg.native_serial_str, entry->disk_change_msg.nsid, entry->disk_change_msg.op);
		nvmeibt_abort(ES_FATAL);
	}

	nvmeibt_topology_setup_relationships();
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(5ghs83j);
	//nvmeibt_global_get_global()->should_send_segment_report = true;
	N_IMf(shyhu76, "EVENT_DISK_CHANGE " LOCAL_DISK_LOG_FMT " op=@OP_CHR",
		  entry->disk_change_msg.disk_id, entry->disk_change_msg.native_serial_str, entry->disk_change_msg.nsid, entry->disk_change_msg.op);

out:
	NFOUT;
}

static void handle_serjio_state_changed(const char* ldisk_id_str, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_status)
{
	// serialize the event
	struct disk_event_wq_entry* event_serjio_state_task;
	struct nvmeibs_toma_disk_change_msg* msg;
	char	ld_display[100];
	struct nvmeibt_ascii_uuid		ldisk_id;

	event_serjio_state_task = NNVMEIBT_BM_CALLOC(u87hdy6, sizeof(*event_serjio_state_task));
	N_Tf(hyuytu7, "got DISK_CHANGE_EVENT from SERJIO state=@INT, disk=@STR", serjio_status, ldisk_id_str);
	event_serjio_state_task->wq_entry.type = "DISK_CHANGE_EVENT";
	event_serjio_state_task->wq_entry.execute = disk_change_event_wrapper;
	event_serjio_state_task->wq_entry.finalize = disk_change_event_finalize;
	event_serjio_state_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	event_serjio_state_task->wq_entry.free = disk_change_event_freer;

	msg = &event_serjio_state_task->disk_change_msg;
	msg->vendor_id = vendor_id;
	nvmeibt_strlcpy(msg->disk_id, ldisk_id_str, sizeof(msg->disk_id));
	nvmeibt_strlcpy(msg->model_str, model_str, sizeof(msg->model_str));
	msg->metadata = serjio_status;
	msg->op = 's';

	nvmeibt_strlcpy(ldisk_id.str, ldisk_id_str, sizeof(ldisk_id.str));
	snprintf(ld_display, sizeof(ld_display), "%.36s(%.24s.%d)", ldisk_id.str, msg->native_serial_str, msg->nsid);
	if (nvmeibt_toma_local_disk_specific_add_work(&ldisk_id, ld_display, &event_serjio_state_task->wq_entry) != 0) {
		N_Ef(ytu7687, "Unable to add hardware event for disk=@STR to WQ", ld_display);
		nvmeibt_abort(ES_FATAL);
	}
}

static int server_handle_local_event(struct nvmeibs_toma_server_proc_buf *msg_buf, int size)
{
	int rv = 0;

	NFIN;

	if (size != sizeof(*msg_buf)) {
		N_Ef(mji889i, "Failed read too short @SIZE", size);
		rv = -1;
		goto out;
	}

	NLOCAL_SERVER_MSG_DUMP(trace_topology_server_handle_local_event, msg_buf);

	switch (msg_buf->type) {
	case NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE: {
			N_Tf(agty675, "got EVENT_DISK_CHANGE disk=@STR op=@OP_CHR - pre-netlink-code - ignoring", msg_buf->disk_change_msg.disk_id, msg_buf->disk_change_msg.op);
			// Ignored - moved to netlink-based drive flow
			break;
		}
	case NVMEIBS_TOMA_REPORT_EVENT_SERJIO_RANGE_CLEANED:
		nvmeibt_seg_active_update_serjio_range_cleaned(msg_buf->serjio_range_cleaned_msg.seg_id);
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_PORT_GID_CHANGE: {
			struct nvmeibt_csv_file_ctx cfg_file = {NICS__INFO_FILE, NVMEIBT_CSV_TYPE_LOCAL_NICS};
			N_IMf(jjuu88w, "EVENT_GID_CHANGE gid=@GID_STR", msg_buf->port_gid_change_msg.gid_str);
			if (nvmeibt_topology_probe_local_hardware(&cfg_file, 1) < 0) {
				// Failed reading hardware config.
				nvmeibt_abort(ES_FATAL);
			}
			/*
			nvmeibt_nm_handle_local_port_gid_change(nvmeibt_get_nw_node(),
				msg_buf->port_gid_change_msg.ib_dev,
				msg_buf->port_gid_change_msg.port,
				msg_buf->port_gid_change_msg.gid_str);
			*/
			if (1) {
				// Reregsiter Peer NICs because of new listener
				reregister_peer_nics();
				/* Tell TOMA main loop to update the IB FDs due to new listener */
				nvmeibt_toma_mark_is_need_to_update_the_main_select_fds();
			}
			NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(snjwu4i);
		}
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_NIC_CHANGE: {
			struct nvmeibt_csv_file_ctx cfg_file = {NICS__INFO_FILE, NVMEIBT_CSV_TYPE_LOCAL_NICS};
			N_IMf(dfrt654, "EVENT_NIC_CHANGE nic=@NIC_STR active=@ACTIVE",
				msg_buf->nic_change_msg.ib_dev, msg_buf->nic_change_msg.add);
			if (nvmeibt_topology_probe_local_hardware(&cfg_file, 1) < 0) {
				// Failed reading hardware config.
				nvmeibt_abort(ES_FATAL);
			}

			/*
			nvmeibt_nm_handle_local_nic_change(nvmeibt_get_nw_node(),
				msg_buf->nic_change_msg.ib_dev, msg_buf->nic_change_msg.add);
			*/
			/* Tell TOMA main loop to update the IB FDs due to change in NICs */
			nvmeibt_toma_mark_is_need_to_update_the_main_select_fds();
			if (msg_buf->nic_change_msg.add) {
				// Reregsiter Peer NICs because of new listener
				reregister_peer_nics();
			}

			NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(hd92mnd);
		}
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_SUBSCRIBER_CHANGE:
		handle_subscriber_event(&(msg_buf->subscriber_change_msg));
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_CLIENT_DISCONNECT:
		handle_client_disconnect_event(nvmeibt_toma_get_local_server_fd_events(), &msg_buf->client_disconnect_msg_hdr);
		break;
	case NVMEIBS_TOMA_WRITE_STATUS_REQ:
		nvmeibt_toma_write_status_srv_req(msg_buf->status_req_msg.type, msg_buf->status_req_msg.fname,
			msg_buf->status_req_msg.handle, msg_buf->status_req_msg.max_length, msg_buf->status_req_msg.handle_req);
		break;
	case NVMEIBS_TOMA_TRIGGER_JGC:
		nvmeibt_recovery_trigger_local_seg_JGC(msg_buf->trigger_JGC_cmd.disk_segment_urn_uuid_str, msg_buf->trigger_JGC_cmd.disk_id_str);
		break;
	case NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED:
		{
			struct nvmeibs_toma_server_proc_buf ack;
			ack.type = NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED_ACK;
			ack.blkset_recovered_ack_msg.cookie = msg_buf->blkset_recovered_msg.cookie;

			if (nvmeibt_topology_is_HW_config_functional()) {
				ack.blkset_recovered_ack_msg.toma_rv = nvmeibt_seg_active_handle_blkset_recovered(&(msg_buf->blkset_recovered_msg));
			} else {
				N_Wf(ju87661, "Got a BLKSET_RECOVERED before mgmt_config. Ignoring.");
				ack.blkset_recovered_ack_msg.toma_rv = 1;
			}

			if (nvmeibt_toma_send_msg_to_local_server(&ack) < 0) {
				N_Wf(ww77823, "Failed to send BLKSET_RECOVERED_ACK to server (@ERRNO - '@AUTO_ERRNO')", errno);
			}
		}
		break;
	default:
		N_Ef(mjiu876, "unknown msg_buf.type=@TYPE", msg_buf->type);
		break;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibt_topology_handle_local_server_event(bool *is_server_event)
{
	struct nvmeibs_toma_server_proc_buf *msg_buf;
	const int max_len = max(NVMEIB_TOMA_REQ_MAX_LEN, (int) sizeof(*msg_buf));
	int rv = 0;

	NFIN;
	msg_buf = NNVMEIBT_BM_CALLOC(trace_topology_nvmeibt_topology_handle_local_server_event, max_len);
	rv = nvmeibt_toma_get_msg_from_local_server(msg_buf, max_len, is_server_event);
	if (rv < 0) {
		goto out;
	} else if (*is_server_event) {
		rv = server_handle_local_event(msg_buf, rv);
		if (rv < 0)
			N_Ef(hhu7876, "Error reading handling local server event rv=@RV!", rv);
	} else {
		rv = nvmeibt_client_handle_incoming_message(msg_buf, rv);
		if (rv < 0)
			N_Ef(aaski98, "Error reading client incoming message rv=@RV!", rv);
	}
out:
	NNVMEIBT_BM_FREE(trace_1_topology_nvmeibt_topology_handle_local_server_event, msg_buf);

	NFOUT;
	return rv;
}

static int change_disk_event(struct nvmeib_disk_info *disk_info, char op)
{
    struct nvmeibt_local_disk *local_disk;
    struct nvmeibt_local_disk *stock_local_disk;
	struct disk_event_wq_entry	*event_disk_change_task;
	struct nvmeibs_toma_disk_change_msg *msg;
	BOOL is_formatted_with_md;
	BOOL							is_binding_to_nvmeibs;
	struct nvmeibt_client			*client;
	int								n_clients_affected = 0;
	char 							ld_display[100];
	struct nvmeibt_ascii_uuid		ldisk_id;

	NFIN;
	nvmeibt_strlcpy(ldisk_id.str, disk_info->disk_id, sizeof(ldisk_id.str));
	event_disk_change_task = NNVMEIBT_BM_CALLOC(info_change_disk_event_0, sizeof(*event_disk_change_task));
	N_IMf(shdyt76, "got EVENT_DISK_CHANGE disk=@STR op=@OP_CHR - netlink", disk_info->disk_id, op);
	event_disk_change_task->wq_entry.type = "DISK_EVENT";
	event_disk_change_task->wq_entry.execute = disk_change_event_wrapper;
	event_disk_change_task->wq_entry.finalize = disk_change_event_finalize;
	event_disk_change_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	event_disk_change_task->wq_entry.free = disk_change_event_freer;

	snprintf(ld_display, sizeof(ld_display), "%.36s(%.24s.%d)", disk_info->disk_id, disk_info->native_serial_str, disk_info->nsid);
	// The structs should be binary compatible, but there's no guarantee they'll stay that way.
	msg = &event_disk_change_task->disk_change_msg;
	msg->n_blocks = 		disk_info->n_blocks;
	msg->n_hw_blocks =		disk_info->n_hw_blocks;
	msg->vendor_id = 		disk_info->vendor_id;
	nvmeibt_strlcpy(msg->model_str,	disk_info->model_str, sizeof(msg->model_str));
	nvmeibt_strlcpy(msg->native_serial_str,	disk_info->native_serial_str, sizeof(msg->native_serial_str));
	nvmeibt_strlcpy(msg->dev_name,	disk_info->dev_name, sizeof(msg->dev_name));
	nvmeibt_strlcpy(msg->disk_id, 	disk_info->disk_id, sizeof(msg->disk_id));
	nvmeibt_strlcpy(msg->status, 	disk_info->status, sizeof(msg->status));
	msg->block_size = 		disk_info->block_size;
	msg->max_request_size = disk_info->max_request_size;
	msg->seq = 				disk_info->seq;
	msg->nsid = 			disk_info->nsid;
	msg->metadata = 		disk_info->metadata;
	msg->op = 				op;

	if (strncmp(msg->dev_name, dev_dir, 5) != 0) {
		int retcode = snprintf(msg->dev_name, sizeof(msg->dev_name), "/dev/%s", disk_info->dev_name);
		if (retcode<0)
			msg->dev_name[sizeof(msg->dev_name)-1] = 0;		// workaround for GCC 8.1 warnings
	}
	NTOMA_ASSERT(change_disk_event_assert, (msg->status[0] != 0), "Bad status string from server, disk=@STR", ld_display);

	stock_local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, &(nvmeibt_global_get_global()->stock_local_disks_hash));
	if (stock_local_disk) {
		is_formatted_with_md = (stock_local_disk->from_config.smart_info.metadata_size != 0);
		is_binding_to_nvmeibs = nvmeibt_local_disk_is_binding_to_nvmeibs(stock_local_disk);
		if (stock_local_disk->is_being_formatted || is_binding_to_nvmeibs) {
			// We know the flow: it can be formatting or just moving the disk with MD to nvmesh driver
		} else {
			N_Wf(msdklx2, "Unknown flow. stock_local_disk=@STR dev=@STR exists op=@OP_CHR is_being_formatted=@BOOL with_md=@BOOL is_binding_to_nvmeibs=@BOOL",
				 ld_display, msg->dev_name, op, stock_local_disk->is_being_formatted, is_formatted_with_md, is_binding_to_nvmeibs);
		}
	}

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, &(nvmeibt_global_get_global()->local_disks_hash));
	if (local_disk) {
		if (op == 'a') {
			N_Ef(fgyt766, "local_disk exists but op=@OP_CHR", op);
		} else if (op == 'r') {
			nvmeibt_local_disk_mark_is_being_deleted(local_disk);
		}
		// Raise version of disk to stop all existing local_disk related tasks.
		local_disk->CHANGE_EVENT_counters.last_CHANGE_no++;
	}

	// put this message to a queue for the given disk parameters to be executed when it's time comes. (when there are no actives left.)
	if (nvmeibt_toma_local_disk_specific_add_work(&ldisk_id, ld_display, &event_disk_change_task->wq_entry) != 0) {
		N_Ef(fggtyr0, "Unable to add hardware event for " LOCAL_DISK_LOG_FMT " to WQ",
			 disk_info->disk_id, disk_info->native_serial_str, disk_info->nsid);
		nvmeibt_abort(ES_FATAL);
	}
	if (op == 'r') {
		XHASHTABLE_FOR_EACH_SAFE(client, &(nvmeibt_global_get_global()->clients_hash)) {	// By clients and not by reg_ctx, for new reg req
			if (client->local_disk == local_disk) {
				client->local_disk = NULL;
				n_clients_affected += !(client->is_delete_in_the_air);
				client->is_delete_in_the_air = 1;
			}
		}
		N_Tf(0sk3i5m, "n_clients_affected=@INT", n_clients_affected);
	}
	NFOUT;
 	return 0;
}

struct netlink_queue_elem_t {
	struct xdlist link;
	char opcode;
	enum nvmeibs_serjio_status serjio_status;
	union {
		struct nvmeib_disk_info disk_info;
		char dummy_name[256];
		struct nvmeib_nl_msg_to_toma	msg_fr_local_clnt;
	};
};

static XDLIST_DECLARE(, struct netlink_queue_elem_t, link) nl_head;
static pthread_mutex_t nl_guard_mutex;
static bool nl_queue_initialized = false;

static int netlink_queue_push(struct nvmeib_disk_info *disk_info, char opcode, char *dummy_name, enum nvmeibs_serjio_status serjio_status, struct nvmeib_nl_msg_to_toma *msg_fr_local_clnt)
{
	struct netlink_queue_elem_t *elem;

	if (!nl_queue_initialized) {
		N_Ef(gtt6555, "Netlink queue not initialized!");
		return -1;
	}

	elem = (struct netlink_queue_elem_t *) NNVMEIBT_TOMA_CALLOC(trace_netlink_queue_push_2, 1, sizeof(struct netlink_queue_elem_t));

	if (dummy_name)
		nvmeibt_strlcpy(elem->dummy_name, dummy_name, sizeof(elem->dummy_name));
	else if (msg_fr_local_clnt) {
		elem->msg_fr_local_clnt = *msg_fr_local_clnt;
	} else if (disk_info) {
		elem->disk_info = *disk_info;
		elem->serjio_status = serjio_status;
	} else {
		N_Ef(2nsjss8, "No valid arg opcode=@CHAR disk_info=@PTR serjio_status=@INT", opcode, disk_info, serjio_status);
	}
	elem->opcode = opcode;

	pthread_mutex_lock(&nl_guard_mutex);
	XDLIST_ADD_TAIL(&nl_head, elem);
	pthread_mutex_unlock(&nl_guard_mutex);
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_NETLINK, NULL);

	return 0;
}

static void handle_nvmeibs_nl_msg(struct netlink_queue_elem_t *elem)
{
	unsigned char	opcode = (elem->opcode & 0x7f);
	NFIN;
	switch (opcode) {
	case 'S':
		handle_serjio_state_changed(elem->disk_info.disk_id, elem->disk_info.vendor_id, elem->disk_info.model_str, elem->serjio_status);
		break;
	case 'a':
	case 'r':
		change_disk_event(&elem->disk_info, opcode);
		break;
	default:
		N_Ef(fvwgz83, "Unknown op=@CHAR", opcode);
	}
	NFOUT;
}

void nvmeibt_netlink_queue_run(void)
{
	struct netlink_queue_elem_t *elem;
	NFIN;

	if (!nl_queue_initialized) {
		pthread_mutex_init(&nl_guard_mutex, NULL);
		XDLIST_HEAD_INIT(&nl_head);
		nl_queue_initialized = true;
	}

	while (!XDLIST_EMPTY(&nl_head)) {
		pthread_mutex_lock(&nl_guard_mutex);
		elem = XDLIST_FIRST(&nl_head);
		if (!elem) {
			pthread_mutex_unlock(&nl_guard_mutex);
			break;
		}
		XDLIST_DEL(&elem->link);
		pthread_mutex_unlock(&nl_guard_mutex);

		if (elem->opcode == 'C') {	// a msg from nvmeibc (client)
			// handle_nvmeibc_nl_msg(elem);
		} else {	// A msg from nvmeibs (server)
			handle_nvmeibs_nl_msg(elem);
		}
		NNVMEIBT_TOMA_FREE(dgty643, elem);
	}
	NFOUT;
}

int nvmeibt_handle_serjio_state_changed_from_nl_ctx(const char *ldisk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_status)
{
	struct nvmeib_disk_info disk_info = {.vendor_id = vendor_id};
	memcpy(disk_info.disk_id, ldisk_id, sizeof(disk_info.disk_id));
	memcpy(disk_info.model_str, model_str, sizeof(disk_info.model_str));

	return netlink_queue_push(&disk_info, 'S', NULL, serjio_status, NULL);
}

static int nvmeibt_add_disk_event_callback(void *ctx __attribute__((unused)), struct nvmeib_disk_info *disk_info)
{
	return netlink_queue_push(disk_info, 'a', NULL, 0, NULL);
}

static int nvmeibt_remove_disk_event_callback(void *ctx __attribute__((unused)), struct nvmeib_remove_disk *disk_remove_msg)
{
	struct nvmeib_disk_info disk_info;

	memset(&disk_info, 0, sizeof(disk_info));	// Avoid strcpy() in change_disk_event() below
	disk_info.vendor_id = disk_remove_msg->vendor_id;
	nvmeibt_strlcpy(disk_info.disk_id, disk_remove_msg->disk_id, sizeof(disk_info.disk_id));
	nvmeibt_strlcpy(disk_info.status, "Remove", sizeof(disk_info.status));

	return netlink_queue_push(&disk_info, 'r', NULL, 0, NULL);
}

int nvmeibt_add_local_clnt_msg_to_toma_nl_queue(void *msg_fr_local_clnt)
{
	return netlink_queue_push(NULL, 'C', NULL, 0, (struct nvmeib_nl_msg_to_toma *)msg_fr_local_clnt);
}

void nvmeibt_topology_register_disk_events(void)
{
	struct nvmeibt_km_comm *p;
	struct nvmeib_register_change_disk cbs;

	p = nvmeibt_get_srv_comm();
	cbs.on_add_disk = &nvmeibt_add_disk_event_callback;
	cbs.on_remove_disk = &nvmeibt_remove_disk_event_callback;
	nvmeibt_km_comm_register_disk_events(p, &cbs);
}

static void store_config_and_topo_and_gpt_on_disk_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_persistency_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct nvmeibt_persistency_wq_entry, wq_entry);

	entry->rv = -1;
	entry->is_config_persisted = false;
	entry->is_topo_persisted = false;

	if (nvmeibt_write_all_disks_GPTs(entry) < 0) {
		N_Wf(ee4557f, "Error saving GPTs to persistency. WIll continue without those disks");
	}
	if (nvmeibt_raft_save_toma_state_to_persistency(entry) < 0) {
		N_Ef(ss9840v, "Error saving topology to persistency");
		goto out;
	}
	else {
		entry->is_topo_persisted = true;
	}
	N_Tf(ddjiu83, "Successfully saved persistency.");
	entry->rv = 0;
out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void free_write_to_persistency_alloc(struct nvmeibt_persistency_wq_entry *write_to_persistency_task, bool is_aborting_launch)
{
	struct local_disk_info					*ld_info;

	NFIN;
	XDLIST_FOREACH_SAFE(ld_info, &write_to_persistency_task->local_disks_info_hash) {
		if (is_aborting_launch) {
			NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(ru87583, ld_info->local_disk_in_info);
		}
		XDLIST_DEL(&ld_info->link);
		NNVMEIBT_BM_FREE(ggi9884, ld_info);
	}
	NNVMEIBT_BM_FREE(5fdaju3, write_to_persistency_task->follower_persist_buf_full.data_buf);
	NNVMEIBT_BM_FREE(po09o0e, write_to_persistency_task);
	NFOUT;
}

static void store_config_and_topo_and_gpt_on_disk_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_persistency_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct nvmeibt_persistency_wq_entry, wq_entry);
	free_write_to_persistency_alloc(entry, 0);

	NFOUT;
}

static void store_config_and_topo_and_gpt_on_disk_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct nvmeibt_persistency_wq_entry *persistency_entry;
	struct nvmeibt_local_disk			*local_disk;
	struct local_disk_info				*ld_info;
	struct nvmeibt_Buf					*follower_wire_topo_buf = &(nvmeibt_global_get_global()->buf_of_follower_wire_topo);
	bool								is_new_TOPO;
	bool								is_new_TOPO_CONFIG;
	bool								is_new_KAFKA_MGMT_CONFIG;
	bool								is_new_RAFT_MEMBERS;
	bool								is_new_RAFT_MEMBERS_SEQ_NO;
	bool								is_new_current_raft_TERM;

	NFIN;
	persistency_entry = container_of(wq_entry, struct nvmeibt_persistency_wq_entry, wq_entry);
	/* if wq_entry was canceled, then set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(48whd8b, "canceled persistency save");
		persistency_entry->rv = -1;
	}
	if (persistency_entry->rv == 0) {
		is_new_TOPO = (RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_submitted));
		is_new_TOPO_CONFIG = (RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_submitted));
		is_new_KAFKA_MGMT_CONFIG = (RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_submitted));
		is_new_RAFT_MEMBERS = (RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_submitted));
		is_new_RAFT_MEMBERS_SEQ_NO = (RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_submitted));
		is_new_current_raft_TERM = (RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_submitted));
		N_Tf(rcahgaw, "is_new?(TOPO?@BOOL TOPO_CONFIG?@BOOL KAFKA_MGMT_CONFIG?@BOOL RAFT_MEMBERS?@BOOL RAFT_MEMBERS_SEQ_NO?@BOOL current_raft_term?@BOOL",
			 is_new_TOPO ,is_new_TOPO_CONFIG ,is_new_KAFKA_MGMT_CONFIG ,is_new_RAFT_MEMBERS ,is_new_RAFT_MEMBERS_SEQ_NO ,is_new_current_raft_TERM);
		//
		SET_RAFT_COMMIT_LIFECYCLE_VAL(rcqeytf, TOPO,                follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,                follower_submitted));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(rc3c5sg, TOPO_CONFIG,         follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG,         follower_submitted));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(7n9efyf, KAFKA_MGMT_CONFIG,   follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG,   follower_submitted));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(rbc6ska, RAFT_MEMBERS,        follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS,        follower_submitted));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(tvahkwe, RAFT_MEMBERS_SEQ_NO, follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_submitted));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(8os2gan, current_raft_TERM,   follower_committed, RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM,   follower_submitted));
		// Parse the committed content into committed
		TODO(Unite with nvmeibt_raft_read_persistence_and_upd_committed());
		if (is_new_KAFKA_MGMT_CONFIG) {
			nvmeibt_topology_parse_a_config(NVMEIBT_CSV_TYPE_FULL_KAFKA_MGMT_CONFIG_VOLUMES, NULL);
		}
		if (is_new_TOPO_CONFIG) {
			nvmeibt_topology_parse_a_config(NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES, NULL);
		}
		if (is_new_TOPO || is_new_TOPO_CONFIG) {
			nvmeibt_topology_parse_committed_topology();
		}
		//
		nvmeibt_topology_active_mark_reserialization_required();	// some "follower committed" values changed, and need to get to the leader.
		// Slightly inefficient. First serialize into the topo_buf, and then copy it into the follower_to_leader_wire_buf
		//  Memory wise, I could first allocate the follower_to_leader_wire_buf and serialize the topo in there
		// Following the Leader's concept that the buffer is an assembly of several buffers that are serialized beforehand
		NNVMEIBT_TOMA_FREE(6dfbsoe, nvmeibt_raft_get_my_raft()->follower_to_leader_wire_buf);
		nvmeibt_raft_get_my_raft()->follower_to_leader_wire_buf = nvmeibt_raft_generate_persist_and_wire_buf(
			nvmeibt_raft_get_current_term(),
			RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_committed),
			nvmeibt_kafka_get_kafka_mgmt_zone_number(),
			&(nvmeibt_raft_get_my_raft()->voted_for_uuid),
			nvmeibt_global_get_mgmt_DB_uuid(),
			nvmeibt_raft_get_persistent_leader_append_entries_time_ns(),	// The follower can only echo back its persistent value
			nvmeibt_raft_get_persistent_leader_topo_calc_time_ns(),	// The follower can only echo back its persistent value
			nvmeibt_raft_get_guaranteed_sw_ver(),
			RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed), -1, follower_wire_topo_buf->data_buf, follower_wire_topo_buf->buf_len,
			RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed), -1, NULL, 0,
			RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed), -1, NULL, 0,
			RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed), RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed), NULL, 0);
		nvmeibt_raft_align_members_with_committed_wire_buf(NULL);	// With RAFT_MEMBERS, a committed list will be used as if "applied" if I reboot. The leader will require a majority of them
	}
	else { /* An error occured during save, now restore the relevant flags.*/
		// Forget the failed submission
		SET_RAFT_COMMIT_LIFECYCLE_VAL(vbd74hs, TOPO,                follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,                follower_committed));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(djxi3ko, TOPO_CONFIG,         follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG,         follower_committed));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(cv6cmwi, KAFKA_MGMT_CONFIG,   follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG,   follower_committed));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(m3diuf1, RAFT_MEMBERS,        follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS,        follower_committed));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(syirwlk, RAFT_MEMBERS_SEQ_NO, follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(zu4dyrp, current_raft_TERM,   follower_submitted, RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM,   follower_committed));
	}
	// Go over all the disks and check if they were persisted, if so update their info in the local_disk object
	XDLIST_FOREACH_SAFE(ld_info, &persistency_entry->local_disks_info_hash) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&(ld_info->from_config.ldisk_id), &(nvmeibt_global_get_global()->local_disks_hash));
		if (!nvmeibt_local_disk_is_being_deleted(local_disk)) {
			if (ld_info->is_gpt_written) {
				local_disk->is_mbr_a_valid_pmbr = 1;
				local_disk->main_gpt.is_valid = 1;
				local_disk->metadata_gpt.is_valid = 1;
				if (!strcmp(local_disk->from_config.status, "Formatting")) {
					sprintf(local_disk->from_config.status, "Initializing");
				}
			} else {
				N_Wf(xhs8ik2, "GPT not written for disk=@STR", nvmeibt_local_disk_display(local_disk));
				local_disk->is_mbr_a_valid_pmbr = 0;
				local_disk->main_gpt.is_valid = 0;
				local_disk->metadata_gpt.is_valid = 0;
				if (ld_info->gpt_change_no == local_disk->gpt_change_no) {
					// Failed to write the current version, It was this task's role. Re-need to write.
					NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(juyr812, local_disk);
				}
			}
			N_Tf(furiw96, "disk=@STR after persistency_save of, ld_info store statuses gpt_change_no=@GPT_CHANGE_NO",
				 nvmeibt_local_disk_display(local_disk), ld_info->gpt_change_no);
		}
		else {
			N_Wf(utieof3, "disk=@STR is not found after persistency_save, skipping updating store status.", local_disk_info_display(ld_info));
		}
	}
	// Only after finalizing the save and after effects, including the COMMITTED_LAST_LOG_INDEX
	// We must dec the n_stores, even if the store failed, since this is a counter of the stores in the
	// air, and this store is no longer in the air, even if it failed.
	NNVMEIBT_GLOBAL_DEC_N_STORES_IN_PROGRESS(frrt225);
	nvmeibt_raft_apply_committed_config_and_topo_as_needed();
	XDLIST_FOREACH_SAFE(ld_info, &persistency_entry->local_disks_info_hash) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ld_info->from_config.ldisk_id, &(nvmeibt_global_get_global()->local_disks_hash));
		if (nvmeibt_local_disk_is_ready_for_segments(local_disk) && nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
			nvmeibt_register_open_disk_eligible_seg_actives_for_use(local_disk);
		}
	}
	if (persistency_entry->rv == 0) {
		struct nvmeibt_persist_and_wire_buf		*persist_and_wire_buf = (struct nvmeibt_persist_and_wire_buf *)(persistency_entry->follower_persist_buf_full.data_buf);
		union nvmeib_uuid						dst_node_uuid = persist_and_wire_buf_get_raft_voted_for_uuid(persist_and_wire_buf);
		struct nvmeibt_node						*dst_node = nvmeibt_node_get_node_by_id(&dst_node_uuid);

		nvmeibt_raft_try_to_send_spurious_APPEND_ENTRIES_REP_or_REQ_VOTE_REP(dst_node, persistency_entry->is_req_vote);
	}
	NFOUT;
}

struct seg_metadata_ctrl_save_wq_entry {
	/* WQ infrastructure */
	struct nvmeibt_wq_entry									wq_entry;
	/* Input parameters */
	struct seg_persistency_write_params						write_params;
	/* Output parameters - used by the thread completion*/
	BOOL													rv;
};


static void seg_metadata_ctrl_save_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct seg_metadata_ctrl_save_wq_entry  *entry = container_of(wq_entry, struct seg_metadata_ctrl_save_wq_entry, wq_entry);

	// NFIN
	N_Tf(t_xx_303, "seg=@UUID_8", nvmeibt_seg_active_UUID_8(entry->write_params.seg_active));
	// Registrable or RAFT-only must be initialized
	entry->rv = nvmeibt_ds_metadata_ctrl_blk_write(&entry->write_params);
	// NFOUT;
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
}

static void seg_metadata_ctrl_save_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct seg_metadata_ctrl_save_wq_entry *entry = container_of(wq_entry, struct seg_metadata_ctrl_save_wq_entry, wq_entry);

	NFIN;
	NNVMEIBT_BM_FREE(wevus83, entry);
	NFOUT;
}

static void seg_metadata_ctrl_save_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct seg_metadata_ctrl_save_wq_entry  *entry = container_of(wq_entry, struct seg_metadata_ctrl_save_wq_entry, wq_entry);
	struct nvmeibt_seg_active				*seg_active;

	NFIN;

	seg_active = entry->write_params.seg_active;
	if (!seg_active) {
		N_Ef(ax8w43e, "seg_active=NULL");
		nvmeibt_abort(ES_FATAL);
	}

	NNVMEIBT_GLOBAL_DEC_N_STORES_IN_PROGRESS(vetywfw);
	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(vseislk, seg_active, "METADATA_SAVE", -1);
	if (nvmeibt_seg_active_final_free_if_not_in_use(seg_active))
		goto out;

	/* if wq_entry was canceled, then set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(4n487w1, "canceled metadata ctrl save seg=@UUID_8", nvmeibt_seg_active_UUID_8(seg_active));
		entry->rv = -1;
		goto out;
	}

	seg_active->persistent_metadata->is_written_on_disk = NVMEIBT_DSEG_MD_CTRL_BLK_STATUS_ON_DISK_VALID;
	NVMEIBT_SEG_ACTIVE_SET_COMMITTED_RESERVATION_MODE_VERSION(njktjtr, seg_active, seg_active->submitted_reservation_mode_version);
	nvmeibt_register_clients_sync_check_and_act_upon(seg_active);

	if (seg_active->was_launch_metadata_store_called_during_metadata_store) {
		nvmeibt_register_launch_seg_metadata_ctrl_save(seg_active);
	}

out:
	NFOUT;
}

int nvmeibt_register_launch_seg_metadata_ctrl_save(struct nvmeibt_seg_active *seg_active)
{
	struct seg_metadata_ctrl_save_wq_entry          *wq_entry;
	const struct nvmeibt_disk_gpt_partition_entry   *seg_metadata_gpt_entry;
	int                                             rv;
	struct nvmeibt_local_disk                       *local_disk;
	struct nvmeibt_disk_segment						*disk_segment;
	struct nvmeibt_praid_topo_ctx					*praid_topo_ctx;
	u64												prev_submitted_reservation_mode_version;

	NFIN;
	// Need to save in any of these cases
	// - When creating a seg
	// - On clean shutdown
	// - On seg_active->highest_reservation_mode_version change
	// - In addition, the seg_active->persistent_metadata is written directly During zeroing
	NTOMA_ASSERT(ygw83ug, seg_active, "seg_active==NULL");
	disk_segment = nvmeibt_seg_active_get_disk_segment(seg_active);
	if (!disk_segment) {
		N_Tf(rbak23l, "Skipping seg_active=@UUID_8 no disk_segment, probably prior to applying topo_config", nvmeibt_seg_active_UUID_8(seg_active));
		rv = 0;
		goto out;
	}
	praid_topo_ctx = nvmeibt_disk_segment_get_praid_applied_topo(disk_segment);
	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(cvwyw4k, "local_disk=@STR is_being_deleted, skipping", nvmeibt_local_disk_display(local_disk));
		rv = 0;
		goto out;
	}

	if (!nvmeibt_disk_segment_is_config_OK(disk_segment)) {
		N_Tf(cvd73ls, "Skiping seg=@UUID_8, it is_being_deleted=@IS_BEING_DELETED is_config_OK=@BOOL", nvmeibt_seg_UUID_8(disk_segment),
			nvmeibt_seg_lot_is_deleted_in_config(&disk_segment->seg_follower.applied_seg_lot), nvmeibt_disk_segment_is_config_OK(disk_segment));
		rv = 0;
		goto out;
	}
	if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(disk_segment)) {
		N_Tf(cvd78d6, "Skiping outdated seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		rv = 0;
		goto out;
	}
	if (!seg_active->persistent_metadata) {
		N_Tf(xvgas72, "Initializing new persistent_metadata for seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
		seg_active->persistent_metadata = NNVMEIBT_BM_ALIGNED_CALLOC(64hsjd8, PAGE_SIZE, SECTOR_SIZE);

		if (nvmeibt_seg_active_metadata_ctrl_init(seg_active) < 0) {
			N_Ef(bcdhjd7, "Error initialization ctrl_blk for seg=@UUID_8 skipping seg", nvmeibt_seg_UUID_8(disk_segment));
			rv = -1;
			goto out;
		}
	}
	// It is important to remove the is_last_shutdown_clean before opening to registration. The rest is less important.
	N_Tf(beuduj3, "is_written_on_disk=@INT is_current_shutdown_clean=@BOOL",
		 seg_active->persistent_metadata->is_written_on_disk,
		 seg_active->persistent_metadata->is_current_shutdown_clean);
	N_Tf(b6fjhj3, "@INT ?? @INT @INT ?? @INT highest_@RES_MOD_VER ?? submitted_@RES_MOD_VER",
		 seg_active->persistent_metadata->active_praid_version_major, praid_topo_ctx->praid_version_major,
		 seg_active->persistent_metadata->active_praid_version_minor, praid_topo_ctx->praid_version_minor,
		 seg_active->highest_reservation_mode_version, seg_active->submitted_reservation_mode_version);
	if (nvmeibt_seg_active_is_during_persistency_store(seg_active)) {
		N_Tf(ebdoxse, "seg=@UUID_8 is_during_metadata_store", nvmeibt_seg_UUID_8(disk_segment));
		seg_active->was_launch_metadata_store_called_during_metadata_store = 1;
		rv = 1;
		goto out;
	}
	seg_active->was_launch_metadata_store_called_during_metadata_store = 0;
	seg_metadata_gpt_entry = nvmeibt_seg_active_get_metadata_gpt_entry(seg_active);
	if (!seg_metadata_gpt_entry) {
		N_Ef(3b2v7s9, "Missing seg metadata for seg=@UUID_8 skipping seg", nvmeibt_seg_UUID_8(disk_segment));
		rv = -1;
		goto out;
	}

	prev_submitted_reservation_mode_version = seg_active->submitted_reservation_mode_version;
	N_Tf(fns93n4, "seg=@UUID_8 writing ctrl_blk", nvmeibt_seg_UUID_8(disk_segment));
	wq_entry = NNVMEIBT_BM_CALLOC(njerjk2, sizeof(*wq_entry));
	wq_entry->write_params.fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	seg_active->persistent_metadata->is_current_shutdown_clean = 0;
	TODO(The following/previous use of applied_topo_ctx is premature, since we didnt yet parse the topo);
	seg_active->persistent_metadata->active_praid_version_major = praid_topo_ctx->praid_version_major;
	seg_active->persistent_metadata->active_praid_version_minor = praid_topo_ctx->praid_version_minor;
	wq_entry->write_params.pblk_size = local_disk->from_config.pblk_size;
	wq_entry->write_params.live_seg_persistent_metadata = seg_active->persistent_metadata;
	NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(3nd8s98, seg_active, seg_active->highest_reservation_mode_version);
	wq_entry->write_params.seg_active = seg_active;
	wq_entry->write_params.num_blksets = num_blksets_in_disk_segment(disk_segment);
	wq_entry->write_params.seg_metadata_pba_s = seg_metadata_gpt_entry->pba_s;
	if (wq_entry->write_params.seg_metadata_pba_s == 0) {
		nvmeibt_abort(ES_FATAL);
	}
	NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(edhwu83, seg_active, "METADATA_SAVE", 1);
	NNVMEIBT_GLOBAL_INC_N_STORES_IN_PROGRESS(vetyw62);

	wq_entry->wq_entry.type = "TYPE_SEGMENT_RESET_MD";
	wq_entry->wq_entry.execute = seg_metadata_ctrl_save_wrapper;
	wq_entry->wq_entry.finalize = seg_metadata_ctrl_save_finalize;
	wq_entry->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	wq_entry->wq_entry.free = seg_metadata_ctrl_save_freer;

	if (nvmeibt_seg_metadata_ctrl_save_add_work(nvmeibt_local_disk_UUID(local_disk), nvmeibt_local_disk_display(local_disk), &(wq_entry->wq_entry)) != 0) {
		N_Ef(4uhw783, "seg=@UUID_8 Failed nvmeibt_seg_metadata_ctrl_save_add_work", nvmeibt_seg_UUID_8(disk_segment));
		rv = -1;
		NVMEIBT_SEG_ACTIVE_SET_SUBMITTED_RESERVATION_MODE_VERSION(brys74h, seg_active, prev_submitted_reservation_mode_version);
		NNVMEIBT_SEG_ACTIVE_UPDATE_REF_COUNT(egsy4m4, seg_active, "METADATA_SAVE", -1);
		NNVMEIBT_GLOBAL_DEC_N_STORES_IN_PROGRESS(3v4y46s);
		goto out;
	}
	rv = 0;
out:
	if (rv != 0) {
		N_Tf(9e8djkl, "seg=@UUID_8 rv=@INT", nvmeibt_seg_active_UUID_8(seg_active), rv);
	}
NFOUT;
	return rv;
}

/* should be matched with free_write_to_persistency_alloc()? */
TODO(Separate, Used to save both the seg_metadata+GPT on the data disks and the raft_persistency on the system disk, these are different workflows)
bool nvmeibt_topology_add_persistency_save_wq_item(bool is_req_vote)
{
	bool									is_topo_save_submitted_for_all = 1;
	struct nvmeibt_persistency_wq_entry		*write_to_persistency_task;
	struct nvmeibt_local_disk				*local_disk;
	struct local_disk_info					*ld_info;
	struct nvmeibt_topology					*cur_topo = nvmeibt_global_get_global();

	NFIN;
	// Allocate task to offload persistency storing of toma config/topo to a thread.
	write_to_persistency_task = NNVMEIBT_BM_CALLOC(ddjj982, sizeof(*write_to_persistency_task));
	write_to_persistency_task->is_req_vote = is_req_vote;
	XDLIST_HEAD_INIT(&write_to_persistency_task->local_disks_info_hash);
	// part of the task that writes on the system disk
	write_to_persistency_task->wq_entry.type = "SAVE_PERSISTENCY";
	write_to_persistency_task->wq_entry.execute = store_config_and_topo_and_gpt_on_disk_wrapper;
	write_to_persistency_task->wq_entry.finalize = store_config_and_topo_and_gpt_on_disk_finalize;
	write_to_persistency_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	write_to_persistency_task->wq_entry.free = store_config_and_topo_and_gpt_on_disk_freer;
	TODO(Separate the store to system-disk from the store to segments metadata (+ disks GPT));
	// For local disks, update added/removed segs, I.e., their GPTs
	XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
		struct nvmeibt_disk *its_disk = NULL;

		if (!local_disk || !nvmeibt_disk_is_local_in_config(NNVMEIBT_LOCAL_DISK_GET_DISK(nvmeibt_topology_add_persistency_save_wq_item_trace_disk, local_disk))) {
			N_Tf(t776yr4, "disk=@STR is not local in config, skipping", nvmeibt_local_disk_display(local_disk));
			continue;	// Logically, the disk is not local. Still here due to temp leftovers (connected clients)
		}
		if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
			N_Tf(dhy7661, "disk=@STR is being deleted, skipping", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		if (nvmeibt_disk_is_explicitly_out_of_service(local_disk->its_disk)) {
			N_Tf(unynf64, "disk=@STR is out of service, skipping", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		if (local_disk->gpt_change_no <= local_disk->gpt_submitted_change_no) { // no need to save GPT
			N_Tf(jjttu22, "disk=@STR GPT was not changed", nvmeibt_local_disk_display(local_disk));
			continue;
		}

		its_disk = NNVMEIBT_LOCAL_DISK_GET_DISK(nvmeibt_topology_add_persistency_save_wq_item_trace_4, local_disk);
		if (!nvmeibt_local_disk_is_ready_for_segments(local_disk) || !nvmeibt_local_disk_is_connected_to_disk(local_disk)) {
			// We still need to persist the disk_version, and only after done, this new
			// disk_version will be also reported to mgmt.
			// Originally intended to solves a race in evict between report_target and report_segments, but seems true
			if (its_disk && its_disk->n_segments) {
				is_topo_save_submitted_for_all = 0;
				N_Tf(ju88er4, "disk=@STR has segments, but is not ready, do nothing", nvmeibt_local_disk_display(local_disk));
				break;
			}
			N_Tf(asd983h, "disk=@STR Not is_ready_for_segments yet, skipping", nvmeibt_local_disk_display(local_disk));
			continue;
		}
		// Include the disk - create a relevant disk_info for it to pass to the task.
		ld_info = NNVMEIBT_BM_CALLOC(trace_nvmeibt_topology_add_persistency_save_wq_item, sizeof(*ld_info));
		nvmeibt_local_disk_fill_ld_info(ld_info, local_disk);
		// Updated the local_disk's "SUBMITTED" only just before submition, when there is no way back
		NNVMEIBT_LOCAL_DISK_SET_GPT_SUBMITTED_CHANGE_NO(cghyu76, local_disk, local_disk->gpt_change_no);

		// Add disk info object with copies of all the data that it needs to the persistency task.
		XDLIST_ADD_TAIL(&write_to_persistency_task->local_disks_info_hash, ld_info);
		N_Tf(fgty767, "added disk=@STR to persistency save, gpt_change_no=@INT", nvmeibt_local_disk_display(local_disk), ld_info->gpt_change_no);
	}
	/* if replay: skip (only do leader's logic and topology calculation) */
	if (nvmeibt_replay_is_enabled())
		goto skip;
	NNVMEIBT_GLOBAL_INC_N_STORES_IN_PROGRESS(dhu8821);
	// Phase 2 - save to persistency, AFTER it is done, updated commit_last_LOG_index.
	nvmeibt_raft_fill_persistence_buf_for_system_disk(&(write_to_persistency_task->follower_persist_buf_full));
	if (is_topo_save_submitted_for_all && nvmeibt_toma_persistency_add_work(&(write_to_persistency_task->wq_entry)) != 0) {
		N_Ef(error_topology_nvmeibt_topology_add_persistency_save_wq_item, "Unable to add persistency offload task to WQ!");
		is_topo_save_submitted_for_all = 0;
	}
	if (!is_topo_save_submitted_for_all) {
		free_write_to_persistency_alloc(write_to_persistency_task, 1);
		NNVMEIBT_GLOBAL_DEC_N_STORES_IN_PROGRESS(dhy778e);
	} else {
		// Sleep a little, so that in MOST cases, the offload thread will finish its work, and the
		// response we'll send to our peer will already include the updated COMMITTED_LAST_LOG_INDEX.
		// Only in cases where the offloading takes a long time, we'll actually have the update happen
		// later than sending of the append_entries_rep, in this iteration - and will send the updated
		// reply in one of the next iterations.
		nanosleep(&(struct timespec) { 0, max(nvmeibt_raft_get_effective_heartbeat_timeout_ns() / 25, MSEC_TO_NSEC(6))}, NULL);	// No need to rush it, the leader will start working on the
																																//  next heartbeat (in ~200ms) anyhow
	}
skip:
	NFOUT;
	return !is_topo_save_submitted_for_all;
}

int nvmeibt_topology_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_Buf		wire_topo = {(size_t)0, (void *)0};

	(*printf_fn)(printf_ctx, "\n- - - - -   GLOBAL TOPOLOGY   - - - - -\n");
	wire_topo.buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full, TLV_TYPE_TOPO_FULL, (char **)&(wire_topo.data_buf));
	if (wire_topo.buf_len) {
		nvmeibt_topology_print(printf_fn, printf_ctx, &wire_topo, 0);
	}
	return 0;
}

bool is_disk_match_spec(target_drives_spec_t *drives_spec_list, struct nvmeibt_ascii_uuid *native_serial, unsigned int vendor_id, char *model_str, int nsid)
{
	struct target_drive	*drive_spec_line;
	bool rv = 0;

	// Is match if ALL the specified arguments in the exluded_line match (ignoring NULL/0 arguments)
	XDLIST_FOREACH_SAFE(drive_spec_line, drives_spec_list) {
		if (drive_spec_line->model_str[0] && (strncmp(drive_spec_line->model_str, model_str, sizeof(drive_spec_line->model_str)) != 0)) {
			continue;
		}
		if (drive_spec_line->native_serial.str[0] && (strncmp(drive_spec_line->native_serial.str, native_serial->str, sizeof(drive_spec_line->native_serial.str)) != 0)) {
			continue;
		}
		if (drive_spec_line->nsid && (drive_spec_line->nsid != (unsigned int)nsid)) {
			continue;
		}
		if (drive_spec_line->vendor && (drive_spec_line->vendor != vendor_id)) {
			continue;
		}
		// All the non-NULL fields match
		rv = 1;
		break;
	}
	return rv;
}

bool nvmeibt_topology_is_disk_explicitly_excluded(struct nvmeibt_ascii_uuid *native_serial, unsigned int vendor_id, char *model_str, int nsid)
{
	bool is = is_disk_match_spec(&(nvmeibt_global_get_global()->excluded_drives_spec), native_serial, vendor_id, model_str, nsid);
	N_Tf(fhu4477, "is=@BOOL serial=@STR vendor=@UINT model='@STR' nsid=@INT", is, native_serial->str, vendor_id, model_str, nsid);
	return is;
}

bool nvmeibt_topology_is_disk_explicitly_auto_takeover(struct nvmeibt_ascii_uuid *native_serial, unsigned int vendor_id, char *model_str, int nsid)
{
	bool is = is_disk_match_spec(&(nvmeibt_global_get_global()->auto_takeover_drives_spec), native_serial, vendor_id, model_str, nsid);
	N_Tf(fhu4488, "is=@BOOL serial=@STR vendor=@UINT model='@STR' nsid=@INT", is, native_serial->str, vendor_id, model_str, nsid);
	return is;
}

void nvmeibt_topology_free_resources(void)
{
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_raft_ctx		*my_raft = nvmeibt_raft_get_my_raft();

	NNVMEIBT_TOMA_FREE(8dj3kss, my_raft->follower_to_commit_persist_and_wire_buf_full);
	// NNVMEIBT_TOMA_FREE(vbs8sl3, my_raft->follower_to_submit_persist_and_wire_buf_full);
	// NNVMEIBT_TOMA_FREE(0vdnzlw, my_raft->follower_submitted_persist_and_wire_buf_full);
	NNVMEIBT_TOMA_FREE(c5c84k4, my_raft->leader_to_commit_persist_and_wire_buf_full);
	NNVMEIBT_TOMA_FREE(c7colp2, my_raft->leader_to_commit_persist_and_wire_buf_topo_only);
	NNVMEIBT_BM_FREE(y3mzpuq, my_raft->leader_to_commit_wire_topo.data_buf);
	NNVMEIBT_BM_FREE(u76yvw2, my_raft->leader_to_commit_wire_topo_config.data_buf);
	NNVMEIBT_BM_FREE(vivmfw0, my_raft->leader_to_commit_wire_kafka_mgmt_config.data_buf);
	NNVMEIBT_BM_FREE(ycnks48, my_raft->leader_to_commit_wire_raft_members.data_buf);
	NNVMEIBT_BM_FREE(kiu12qa, cur_topo->buf_of_follower_wire_topo.data_buf);
	NNVMEIBT_BM_FREE(etnpa51, raft_long_msg_test_buf.data_buf);
	HW_conf_free_tree(cur_topo->HW_mgmt_conf);
}

void nvmeibt_topology_set_raft_long_msg_test_appendix_len(int appendix_len)
{
	struct raft_long_msg_test	*appendix_ptr;

	// First of all restore the buffer if it was changed
	if (raft_long_msg_test_appendix_len) {
		appendix_ptr = (struct raft_long_msg_test *)((char *)(raft_long_msg_test_buf.data_buf) + raft_long_msg_test_appendix_len - sizeof(struct raft_long_msg_test));
		memset (appendix_ptr, 'Z', sizeof(struct raft_long_msg_test));
	}

	raft_long_msg_test_appendix_len = appendix_len * 1024;
	N_Tf(t_zz_0,"Set raft long messages test appendix len=@INT Kbytes", appendix_len);

	if (raft_long_msg_test_appendix_len) {
		appendix_ptr = (struct raft_long_msg_test *)((char *)(raft_long_msg_test_buf.data_buf) + raft_long_msg_test_appendix_len - sizeof(struct raft_long_msg_test));
		appendix_ptr->appendix_signature = RAFT_LONG_MSG_TEST_SIGNATURE;
		appendix_ptr->appendix_len = raft_long_msg_test_appendix_len;
	}
}

void nvmeibt_topology_convert_serialized_topo_buf_to_wire(struct nvmeibt_Buf *wire, struct nvmeibt_Buf *serialized)
{
	NNVMEIBT_BUF_RESIZE(gyur75q, wire, serialized->buf_len);
	memset(wire->data_buf, 0, wire->buf_len);
	N_Tf(dgtq12m, "len=@SIZE_T", wire->buf_len);
	nvmeibt_convert_topo_le_be(serialized->data_buf, wire->data_buf, 1, NULL);
}

void nvmeibt_topology_convert_wire_topo_buf_to_serialized(struct nvmeibt_Buf *serialized, const struct nvmeibt_Buf *wire, struct nvmeibt_Str *JSON_output)
{
	N_Tf(dgtqbdo, "len=@SIZE_T", wire->buf_len);
	if (wire->buf_len) {
		NNVMEIBT_BUF_RESIZE(ciurz7q, serialized, wire->buf_len);
		memset(serialized->data_buf, 0, serialized->buf_len);
		nvmeibt_convert_topo_le_be(wire->data_buf, serialized->data_buf, 0, JSON_output);
	}
}

void nvmeibt_topology_init_raft_long_msg_test_buf(void)
{
	// Build dummy buf for raft long messages test. This string is not used without error injection.
	raft_long_msg_test_buf.data_buf = NNVMEIBT_BM_ALLOC(wrnh19l, (size_t)RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN);
	raft_long_msg_test_buf.buf_len = RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN;
	memset (raft_long_msg_test_buf.data_buf, 'Z', RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN);
}

