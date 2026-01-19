/**
 * @file   nvmeibt_topology.h
 * @Author Ronen (ronen@excelero.com)
 * @date   Mar, 2015
 * @brief  The topology data glue
 *
 * The relationships between nodes, clients, drives, ... do
 * not belong in any of the specific  entities
 */

#ifndef NVMEIBT_TOPOLOGY
#define NVMEIBT_TOPOLOGY

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "nvmeibt_params.h"
#include "nvmeibt_common.h"
#include "../common/nvmeib_shared.h"
#include "interfaces/network/network_incs.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "nvmeibt_mm_json.h"

// Acronyms
//  - CT - Client --> Toma
//  - TC - Toma --> Client
//  - TL - Toma --> Leader
//  - LT - Leader --> Toma
//  - WN - Owner --> Not_Owner (Toma to Toma)
//  - NW - Not_Owner --> Owner (Toma to Toma)
//  - RT - Registrant --> Toma
//  - TR - Toma --> Registrant
//  - LB - LoopBack (IB)

#define SRM_USER (1 << 15)
enum NVMEIBT_TOPOLOGY_TOMA_MSG_TYPES {
	// Recovery
	NVMEIBT_TOPOLOGY_MSG_LB_BLKSET_LOCK_REQ 				= 0x70 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY,
	NVMEIBT_TOPOLOGY_MSG_LB_BLKSET_LOCK_RSP 				= 0x71 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY | SRM_USER,

	NVMEIBT_TOPOLOGY_MSG_WN_BLKSET_DATA_SYNC_REQ 			= 0x72 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY,
	NVMEIBT_TOPOLOGY_MSG_NW_BLKSET_DATA_SYNC_RSP 			= 0x73 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY | SRM_USER,

	NVMEIBT_TOPOLOGY_MSG_WN_SYNC_LOCKS_REQ 					= 0x74 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY,
	NVMEIBT_TOPOLOGY_MSG_NW_SYNC_LOCKS_RSP 					= 0x75 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY | SRM_USER,

	NVMEIBT_TOPOLOGY_MSG_TT_OUT_OF_SYNC 					= 0x76 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY,

	NVMEIBT_TOPOLOGY_MSG_TR_FLUSH_RX_Q_CMD	 				= 0x79 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY,
	NVMEIBT_TOPOLOGY_MSG_NW_ERR_DEAD_SEGMENT_RSP			= 0x7A | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY | SRM_USER,

	NVMEIBT_TOPOLOGY_MSG_RT_ERR_RSP							= 0x81 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY | SRM_USER,


	// Volume resize
	// Volume reduce needs to erase the data (trim), so that newly created FS will not crash
	TODO(Not for 1.0, reduce is even more foreward looking)
	NVMEIBT_TOPOLOGY_MSG_LT_BLOCK_DEVICE_RESIZE = 0x80 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA,
	NVMEIBT_TOPOLOGY_MSG_TL_BLOCK_DEVICE_RESIZE_ACK = 0x81 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA,
	NVMEIBT_TOPOLOGY_MSG_LT_PROVISIONING_GROUP_RESIZE = 0x80 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA,
	NVMEIBT_TOPOLOGY_MSG_TL_PROVISIONING_GROUP_RESIZE_ACK = 0x81 | NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA,
};

struct nvmeibt_topology;

#include "nvmeibt_read_config.h"
#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_register.h"
#include "clnt/nvmeibt_client.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_nic.h"
#include "nvmeibt_disk.h"
#include "nvmeibt_node.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_local_nic.h"

/**************************       ------------       **************************/

#define RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN		(8 * 1024 * 1024)
#define	RAFT_LONG_MSG_TEST_SIGNATURE 0x7254535454535472 // '0x72 TSTTST 0x72'
struct raft_long_msg_test {
    long long		appendix_len;
    long long		appendix_signature;
};

struct target_drive {
	struct nvmeibt_ascii_uuid	native_serial;
	unsigned int				vendor;
	char		 				model_str[ASCII_UUID_MAX_STR_LEN];
	unsigned int				nsid;
	struct xdlist 				target_drives_link;
};

struct nvmeibt_topology_serialized_topo_header {
    char								topo_name[NVMEIBT_TOPOLOGY_BIN_NAME_LEN];
    unsigned int						sw_ver;
    unsigned int						topo_len;
	int64_t								UNUSED_was_topo_config_kafka_offset;
    int									praids_num;
	int									res_1;
	int									res_2;
} __attribute__((packed));

struct nvmeibt_active_topo_header {
    char								topo_name[NVMEIBT_TOPOLOGY_BIN_NAME_LEN];
    unsigned int						sw_ver;
    unsigned int						topo_len;
    int									segs_num;
	int									res;
} __attribute__((packed));

struct nvmeibt_topology *nvmeibt_global_get_global(void);

void nvmeibt_topology_leader_mark_all_modified_praids_report_to_mgmt_due_to_committed_by_majority(void);
void nvmeibt_topology_leader_resend_all_praids_report_to_mgmt(void);
int nvmeibt_topology_leader_resend_specific_vol_praids_report_to_mgmt(char *vol_name);
void nvmeibt_topology_mark_update_csv_of_config_and_topo_required(void);
BOOL nvmeibt_topology_leader_is_recalc_required(void);
void nvmeibt_topology_leader_mark_recalc_required(void);
void nvmeibt_topology_leader_clear_recalc_required(void);
BOOL nvmeibt_topology_active_is_reserialization_required(void);
void nvmeibt_topology_active_mark_reserialization_required(void);
void nvmeibt_topology_active_clear_reserialization_required(void);
enum nvmeibt_topology_shutdown_state  nvmeibt_topology_applied_get_shutdown_state(void);
void nvmeibt_topology_applied_mark_shutdown_start(void);
void nvmeibt_topology_applied_mark_shutdown(void);
void nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(struct nvmeibt_disk *disk);
void nvmeibt_topology_remove_disk_from_its_current_node(struct nvmeibt_disk *disk);
void nvmeibt_topology_print_versions(struct nvmeibt_topology_serialized_topo_header *header_ptr);
int update_liveliness_of_seg_actives_of_specific_local_disk(struct nvmeibt_local_disk *local_disk);
int nvmeibt_topology_probe_local_hardware(enum NVMEIBT_CSV_TYPE what);
int nvmeibt_topology_relate_hardware_probe_to_config(void);
int nvmeibt_topology_setup_relationships(void);
int nvmeibt_topology_parse_committed_topology(void);
int nvmeibt_topology_parse_a_config(enum NVMEIBT_CSV_TYPE content_type, struct nvmeibt_Str *JSON_output);
void nvmeibt_topology_apply_the_latest_committed_config_and_topo(void);
int nvmeibt_topology_leader_new_remote_applied_topology_arrived(const void *remote_topology_data, int remote_topology_data_len, struct nvmeibt_raft_member *remote_member);
void nvmeibt_topology_reset_due_to_convert_to_leader(void);
void nvmeibt_topology_serialize_conf_and_topo_if_needed(void);
void nvmeibt_topology_calc_topology(void);
int nvmeibt_topology_serialize_active_topology(void);
void nvmeibt_topology_leader_connect_disk_with_raft_member(struct nvmeibt_disk *disk, struct nvmeibt_raft_member *member);
void nvmeibt_topology_leader_detach_all_disks_from_raft_member(struct nvmeibt_raft_member *member);
void nvmeibt_topology_detach_all_disks_from_node(struct nvmeibt_node *node);
void nvmeibt_topology_detach_all_nics_from_node(struct nvmeibt_node *node);
int nvmeibt_topology_add_seg_active_to_both_mem_gpts(struct nvmeibt_local_disk *local_disk, struct nvmeibt_seg_active *seg_active);
bool nvmeibt_topology_add_persistency_save_wq_item(bool is_req_vote);
int nvmeibt_topology_init(void);
int nvmeibt_topology_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
bool nvmeibt_topology_is_disk_explicitly_excluded(struct nvmeibt_ascii_uuid *native_serial, unsigned int vendor_id, char *model_str, int nsid);
bool nvmeibt_topology_is_disk_explicitly_auto_takeover(struct nvmeibt_ascii_uuid *native_serial, unsigned int vendor_id, char *model_str, int nsid);
void nvmeibt_topology_free_resources(void);
void nvmeibt_topology_set_raft_long_msg_test_appendix_len(int appendix_len);
void nvmeibt_topology_convert_serialized_topo_buf_to_wire(struct nvmeibt_Buf *wire, struct nvmeibt_Buf *serialized);
void nvmeibt_topology_convert_wire_topo_buf_to_serialized(struct nvmeibt_Buf *serialized, const struct nvmeibt_Buf *wire, struct nvmeibt_Str *JSON_output);
void nvmeibt_topology_build_second_global_topo_buf(BOOL is_convert_to_wire);
void nvmeibt_topology_init_raft_long_msg_test_buf(void);
void nvmeibt_topology_leader_serialize_baseline_topo_to_wire(void);
unsigned long long nvmeibt_topology_leader_get_next_config_version(void);

#endif	// #ifndef NVMEIBT_TOPOLOGY

