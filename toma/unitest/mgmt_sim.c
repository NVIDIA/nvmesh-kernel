#include "mgmt_sim.h"
#include "sandbox_util.h"
#include "sandbox_kafka_public.h"

// Toma headers
#include "nvmeibt_json_base.h"
#include "nvmeibt_debug.h"

#define MGMT_DB_UUID_JSON "\"dbUUID\":\"141d3140-c3c0-11f0-bc49-e391b6ca4c2b\""

/*
 * State machine stats for the formatDrive scenario.
 */
 enum mgmt_sim_fsm_state {
	MGMT_FSM_WAITING_FOR_BOTH_OK, // waiting for both disks to report status="Ok"
	MGMT_FSM_SENT_FORMAT_DRIVE, // sent formatDrive message to Toma
	MGMT_FSM_SAW_FORMATTING, // disk reported status="Formatting"
	MGMT_FSM_DONE // test scenario complete - we got the updated disk format in Toma's reportTarget message
};

static int make_msg_update_leader_keepalive_token(char *buf, size_t capacity) {
	return snprintf(buf, capacity, "{\"messageType\":\"updateLeaderKeepaliveToken\""
		",\"messageTypeVersion\":1,\"payload\":{\"token\":1,\"keepaliveInterval\":1}}");
}

/* Format addVolume message */
__attribute__((unused)) // Not yet used - for reference only
static int make_msg_add_volume(char *buf, size_t capacity)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"addVolume\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"_id\":\"V1\",\"uuid\":\"f1b17590-c52b-11f0-bc49-e391b6ca4c2b\""
		",\"version\":1,\"name\":\"V1\",\"blockSize\":4096"
		",\"lockServer\":{\"maxNOwners\":1,\"type\":4,\"locksetShift\":-1}"
		",\"blocks\":4882432,\"RAIDLevel\":\"Striped RAID-0\""
		",\"numberOfMirrors\":0,\"stripeSize\":32,\"stripeWidth\":2,\"status\":\"unavailable\""
		",\"action\":\"initializing\",\"relativeRebuildPriority\":10"
		",\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null"
		",\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,"
		"\"chunks\":["
		"{\"uuid\":\"09c2f550-c52c-11f0-bc49-e391b6ca4c2b\",\"vlbs\":0,\"vlbe\":4882431,\"pRaids\":["
		"{\"uuid\":\"09c2f552-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false"
		",\"stripeIndex\":0,\"zone\":\"1\",\"diskSegments\":["
		"{\"uuid\":\"09c2f551-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":26900224,\"lbe\":29341439"
		",\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\""
		",\"diskUUID\":\"f3a2b830-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
		"{\"uuid\":\"09c34371-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false"
		",\"stripeIndex\":1,\"zone\":\"1\",\"diskSegments\":["
		"{\"uuid\":\"09c34370-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":1509632,\"lbe\":3950847"
		",\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\""
		",\"diskUUID\":\"f3a24300-c3c0-11f0-bc49-e391b6ca4c2b\"}"
		"]}]}]}}");
}

/* Forward declarations */
static void mgmt_sim_parse_report_target(struct mm_json_elem *root);
static void mgmt_sim_run_fsm(void);

/* Per-disk status extracted from reportTarget */
struct mgmt_sim_disk_status {			// Todo: maybe move to cfg?
	const char *disk_id;				// Disk name
	const char *uuid;
	char status[16];					// As reported by Toma
	int64_t format_request_counter;
	int64_t active_format_request_counter;
	u16 vendor;
	u16 block_size;						// In bytes
	u16 metadata_size;					// In bytes
};

static int make_msg_format_drive(char *buf, size_t capacity, const struct mgmt_sim_disk_status *d, unsigned format_request_counter, unsigned long boot_time) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"formatDrive\",\"messageTypeVersion\":1"
		",\"payload\":{\"diskID\":\"%s\",\"uuid\":\"%s\",\"vendor\":%u"
		",\"formatType\":\"format_ec\",\"formatRequestCounter\":%u"
		",\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu"
		", " MGMT_DB_UUID_JSON "}}",
		d->disk_id, d->uuid, d->vendor, format_request_counter, boot_time);
}

/* Management simulator state */
struct mgmt_sim_state {
	struct sb_cluster_conf *cfg;

	struct {							// Per-consumer state for deterministic message sequencing
		int msg_count, conf_version;
	} hw;
	int cmd_msg_count;
	struct t_raft_quorum_config {
		short num_nodes;
		short generation;				// Ever increasing raft domain idx
	} raft_quorum;
	struct t_mgmt_kafka_consumers {		// Toma side producers are mgmt side consumers
		struct sim_broker_topic *high, *kal, *low;
	} k_consumers;
	struct t_mgmt_kafka_producers {		// Toma side consumers are mgmt side producers
		struct sim_broker_topic *hw, *cmd, *l_vol, *l_raft;
	} k_producers;
	int volume_msg_count;

	/* Per-Producer state for deterministic message sequencing */
	int n_leader_keep_alives;
	uint32_t raftTerm;					// AS reported by Toma leader

	/* State machine for formatDrive scenario */
	enum mgmt_sim_fsm_state fsm_state;
	int64_t boot_time;                      /* from reportTarget payload.node.bootTime */
	struct mgmt_sim_disk_status disk_002;   /* NVMD_SN_002.1 */
	struct mgmt_sim_disk_status disk_003;   /* NVMD_SN_003.1 */
};

void sb_cluster_conf_create( struct sb_cluster_conf *sb) {
	int i;
	gethostname(sb->my_hostname, sizeof(sb->my_hostname) - 1);
	sb->n_nodes = (int)ARRAY_SIZE(sb->nodes);
	sb->live =  &sb->nodes[0];
	sb->other = &sb->nodes[1];
	   sb->live->hostname = sb->my_hostname;
	sb->other[0].hostname = "n37@nvidia.com";
	sb->other[1].hostname = "n39@nvidia.com";
	for (i = 0; i < sb->n_nodes; i++) {
		const uint32_t short_uuid = 0xcde269b0 + i;
		sb->nodes[i].uuid16b[0] = short_uuid;
		snprintf(sb->nodes[i].uuid, 37, "%8x-0000-0000-0000-000000000000", short_uuid);
	}
}

int sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *sb, const char *host_name) {
	for (int i = 0; i < sb->n_nodes; i++) {
		if (!strcmp(sb->nodes[i].hostname, host_name))
			return i;
	}
	BUG_ON(true); return -1;
}

void sb_cluster_conf_destroy(struct sb_cluster_conf *sb) {
	(void)sb;
}

static struct mgmt_sim_state *g_mgmt_sim = NULL;

struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg) {
	struct mgmt_sim_state *m = g_mgmt_sim = calloc(1, sizeof(*g_mgmt_sim));
	m->cfg = initialized_cfg;

	/* Initialize state machine */
	m->fsm_state = MGMT_FSM_WAITING_FOR_BOTH_OK;
	m->disk_002.disk_id = "NVMD_SN_002.1";
	m->disk_003.disk_id = "NVMD_SN_003.1";
	m->disk_002.uuid = "d0020000-0000-0000-0000-000000000000";
	m->disk_003.uuid = "d0030000-0000-0000-0000-000000000000";
	m->disk_002.vendor = 5122;
	m->disk_003.vendor = 5123;
	m->hw.conf_version = 17;		// Start from some number
	N_Tf(msim_init, "mgmt_sim initialized cluster @INT machines, hw_conf_ver=@INT", m->cfg->n_nodes, m->hw.conf_version);

	// Connect to kafka
	m->k_consumers.high =   sim_broker_topic_find_by(KTOPIC_TYPE_T2M_PRIORITY);
	m->k_consumers.kal =    sim_broker_topic_find_by(KTOPIC_TYPE_T2M_KEEPALIVE);
	m->k_consumers.low =    sim_broker_topic_find_by(KTOPIC_TYPE_T2M_LOW);
	m->k_producers.hw =     sim_broker_topic_find_by(KTOPIC_TYPE_M2T_HW_CFG);
	m->k_producers.cmd =    sim_broker_topic_find_by(KTOPIC_TYPE_M2T_CMD);
	m->k_producers.l_vol =  sim_broker_topic_find_by(KTOPIC_TYPE_M2T_VOLUMES);
	m->k_producers.l_raft = sim_broker_topic_find_by(KTOPIC_TYPE_M2T_TARGETS_RAFT);
	return m;
}

void mgmt_sim_send_msg_assign_to_zone(int zone_idx) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	const size_t capacity = 256;
	char *msg = malloc(capacity);
	const size_t len = snprintf(msg, capacity,
		"{\"messageType\":\"updateTomaKeepaliveToken\",\"messageTypeVersion\":1"
		",\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"%d\",\"keepaliveInterval\":1}}",
		m->cfg->live->hostname, zone_idx);
	sim_broker_topic_msg_produce(g_mgmt_sim->k_producers.cmd, msg, len, false);
	m->cmd_msg_count++;
}

void mgmt_sim_send_msg_change_raft_quorum(const int node_idx, bool do_add) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	const struct sb_node_conf *node = &m->cfg->nodes[node_idx];
	const char *msg_type = (do_add ? "addTarget" : "deleteTarget");
	const size_t capacity = 256;
	size_t len = 0;
	char *buf = malloc(capacity);	BUG_ON(!buf);
	++m->raft_quorum.generation;
	len = snprintf(buf, capacity, "{\"messageType\":\"%s\",\"messageTypeVersion\":1,\"payload\":"
			"{\"nodeID\":\"%s\",\"uuid\":\"%s\",\"targetsInZone\":%d,\"targetUpdatesSequence\":%d}}",
			msg_type, node->hostname, node->uuid, m->raft_quorum.num_nodes, m->raft_quorum.generation);
	m->raft_quorum.num_nodes += (do_add ? +1 : -1);
	N_Tf(__AUTOID__, "<< msg=@STR node=@STR, gen=@INT", msg_type, node->hostname, m->raft_quorum.generation);
	sim_broker_topic_msg_produce(m->k_producers.l_raft, buf, len, false);
}

void mgmt_sim_send_msg_latest_hw_config(void) {
	const struct mgmt_sim_state *m = g_mgmt_sim;
	const struct sb_node_conf *other_toma = m->cfg->other;
	const size_t capacity = 4096;
	char *msg = malloc(capacity);
	const size_t len = snprintf(msg, capacity,
		"{\"messageType\":\"hardwareConfiguration\",\"messageTypeVersion\":1,\"payload\":{\"managementConfiguration\":{\"_id\":\"1\""
		",\"configurationVersion\":%d,\"leaderToken\":1,\"kafkaMessageSequence\":%d,\"raftTerm\":9"
		",\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},"
		"\"targets\":["
			"{\"_id\":\"nvme37.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"%s\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5122,\"uuid\":\"%s\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"%s\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5123,\"uuid\":\"%s\",\"version\":7,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
			"{\"_id\":\"nvme38.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"D0_n38\",\"blocks\":2000,\"block_size\":4096"
					",\"activeFormatRequestCounter\":1,\"vendorID\":5122"
					",\"uuid\":\"f38cebd0-0000-0000-0000-000000000000\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"D1_n38\",\"blocks\":2000,\"block_size\":4096"
					",\"activeFormatRequestCounter\":1,\"vendorID\":5123"
					",\"uuid\":\"f38cebd1-0000-0000-0000-000000000000\",\"version\":7,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5e008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c1-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5e009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c1-11f0-bc49-e391b6ca4c2b\"}]},"
			"{\"_id\":\"nvme39.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"D0_n39\",\"blocks\":195353046,\"block_size\":4096"
					",\"activeFormatRequestCounter\":1,\"vendorID\":5197"
					",\"uuid\":\"f39cebd0-0000-0000-0000-000000000000\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"D1_n39\",\"blocks\":195353046,\"block_size\":1024"
					",\"activeFormatRequestCounter\":0,\"vendorID\":3333"
					",\"uuid\":\"f39cebd1-0000-0000-0000-000000000000\",\"version\":1,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5f008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0b0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef2-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5f009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0b0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce12-c3c0-11f0-bc49-e391b6ca4c2b\"}]}"
		"]}}",
		m->hw.conf_version, m->hw.msg_count,
		m->cfg->live->hostname, m->cfg->live->uuid, m->disk_002.disk_id, m->disk_002.uuid, m->disk_003.disk_id, m->disk_003.uuid,
		other_toma[0].hostname, other_toma[0].uuid,
		other_toma[1].hostname, other_toma[1].uuid);
	sim_broker_topic_msg_produce(g_mgmt_sim->k_producers.hw, msg, len, false);
}

static void __handle_low_prio_msg(__attribute__((unused)) const rd_kafka_message_t *msg) {
	// {"originType":"TOMA","messageType":"driveZeroingProgress","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":27,"leaderToken":null,"payload":{"zeroWriteCounter":303100870554,"nZeroedBlks":1953536,"diskUUID":"de2eadb0-e972-11f0-8e2a-434e55e1d7f7","node_id":"nvme37.nvidia.com"}}
	// {"originType":"TOMA","messageType":"updateDiskSegmentsDirtyBits","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":97,"leaderToken":null,"payload":{"segmentsDirtyBitsUpdate":[{"pRaidMinorVersion":2,"pRaidMajorVersion":259,"segmentID":"c1105e50-e97b-11f0-995c-3792ee0db955","pRaidUUID":"c1103742-e97b-11f0-995c-3792ee0db955","remainingDirtyBits":67932,"reappearingCounter":3}]}}
	return;	// Todo: handle drive zeroing reports here
}

static void __handle_keepalive_msg(const rd_kafka_message_t *msg) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
		if (strcmp(message_type, "leaderKeepalive") == 0) {
			struct mm_json_elem *payload = json_get_dict_value(root, "payload");
			m->raftTerm = json_get_dict_num(payload, "raftTerm", 0);
			m->n_leader_keep_alives++;
			N_Tf(__AUTOID__, "<< Leader KAL {raftTerm=@INT, gen=@INT}", m->raftTerm, m->n_leader_keep_alives);
		} else if (strcmp(message_type, "keepalive") == 0) {
			// {"originType":"TOMA","messageType":"keepalive","messageTypeVersion":2,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":11831,"leaderToken":null,"keepaliveInterval":5,"payload":{"zone":"1","leaderUUID":"nvme39.nvidia.com","bootTime":1767279770145,"featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":"","rebuildStats":{"nRunningDirtyRebuild":0,"nPendingDirtyRebuild":0,"nRunningStaleRebuild":0,"nPendingStaleRebuild":6,"nRunningTxidRebuild":0,"nPendingTxidRebuild":0,"nRunningColdRecovery":0,"nPendingColdRecovery":0,"nRunningJGCRebuild":0,"nPendingJGCRebuild":0,"nRunningScrubbing":0,"nPendingScrubbing":3}}}
		} else { BUG_ON(true); }
	nvmeibt_mm_json_free_kv_tree(root);
}

static void __handle_priority_msg(const rd_kafka_message_t *msg) {
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
		if (strcmp(message_type, "reportTarget") == 0) {
			// {"originType":"TOMA","messageType":"reportTarget","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":24,"leaderToken":null,"payload":{"node":{"zone":"1","bootTime":1767535819492,"cpu_temp":"30.0","version":"3.3.0-1340","buildNumber":"","reportID":25,"branch":"master","commit":"18af9e759c828cc1d3776bfdd6cb68ade54ef738","configProfile":{"id":"569407c0-e976-11f0-b6cb-871ca30885b4","name":"Cluster Default","version":"1"},"node_status":"1","node_id":"nvme37.nvidia.com","targetUpdatesSequence":4,"cpu_load":"0.0","disks":[{"diskID":"S3HCNX0JC01918.1","disk_version":0,"blocks":1562500000,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01918","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"1_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5f","Power_On_Hours":"0x10049","Unsafe_Shutdowns":"0x56","Media_Errors":"0x2","Number_of_Error_Information_Log_Entries":"0x1a5d","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":25439882564,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700220.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700220","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xb4","Power_On_Hours":"0xfb7f","Unsafe_Shutdowns":"0x94","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x5980","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4546392587,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0JC01904.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01904","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"2_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5e","Power_On_Hours":"0x10046","Unsafe_Shutdowns":"0x54","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x2beb","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":49694267592,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700164.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700164","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x6e","Power_On_Hours":"0x1062c","Unsafe_Shutdowns":"0x5c","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x46","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4675551222,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0K600397.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0K600397","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xca","Power_On_Hours":"0xe767","Unsafe_Shutdowns":"0xb5","Media_Errors":"0x3","Number_of_Error_Information_Log_Entries":"0x165c","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":29489346822,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"disID":"S4C9NF0M500226.1","disk_version":0,"blocks":3125627568,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S4C9NF0M500226","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL1T6HAJQ-00005","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"4_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x56","Power_On_Hours":"0xda5d","Unsafe_Shutdowns":"0x49","Media_Errors":"0x57","Number_of_Error_Information_Log_Entries":"0x1c5a","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":303100870572,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S665NE0R702075.1","disk_version":0,"blocks":1875385008,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S665NE0R702075","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZ1L2960HCJR-00A07","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"3_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x70","Power_On_Hours":"0x817d","Unsafe_Shutdowns":"0x53","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x0","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":10392940702,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0}],"nics":[{"nicID":"0x0000000000000000bae924fffee5cfd8","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0125","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_2"},{"nicID":"0x0000000000000000bae924fffee5cfd9","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0225","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_3"}]}}}
			mgmt_sim_parse_report_target(root);
			mgmt_sim_run_fsm();
		} else  if (strcmp(message_type, "updatePRaidReport") == 0) {
		 	// {"originType":"TOMA","messageType":"updatePRaidReport","messageTypeVersion":1,"hostname":"nvme39.nvidia.com","tomaToken":2,"messageSequence":85,"leaderToken":1,"payload":{"pRaidsUpdate":[{"uuid":"b60b04b1-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60b04b0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60b2bc0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]},{"uuid":"b60ab692-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60ab691-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60adda0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]}]}}
		} else if (strcmp(message_type, "segmentZeroingProgress") == 0) {
			// {"originType":"TOMA","messageType":"segmentZeroingProgress","messageTypeVersion":1,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":335,"leaderToken":null,"payload":{"praidVersion":"258.0","segmentUUID":"98e46d20-ea22-11f0-bad8-af65dd8e6ead","pRaidUUID":"98e44612-ea22-11f0-bad8-af65dd8e6ead","nZeroedBlks":262144}}
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

void mgmt_sim_verify_at_end(void) {
	BUG_ON(!mgmt_sim_is_done() || (g_mgmt_sim->volume_msg_count <= 0) || (g_mgmt_sim->n_leader_keep_alives <= 0));
}

bool mgmt_sim_is_done(void) { return g_mgmt_sim->fsm_state == MGMT_FSM_DONE; }

void mgmt_sim_destroy(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	free(m);
	g_mgmt_sim = NULL;
}

void mgmt_sim_wakeup_on_incomming_toma_msg(struct sim_broker_topic *t) {	// Called on Toma kafka thread
	struct mgmt_sim_state *m = g_mgmt_sim;
	rd_kafka_message_t msg;
	while (sim_broker_topic_msg_consume(t, &msg)) {
		if (     t == m->k_consumers.high)	__handle_priority_msg(&msg);
		else if (t == m->k_consumers.low)	__handle_low_prio_msg(&msg);
		else if (t == m->k_consumers.kal)	__handle_keepalive_msg(&msg);
		sim_broker_topic_ack_offsets(t, msg.offset);
	}
}

void mgmt_sim_do_periodic(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	if (0) { /* 1. Consume Toma's outgoing messages, not done from unitest context but inline from toma context when it sends messages */
		mgmt_sim_wakeup_on_incomming_toma_msg(m->k_consumers.high);
		mgmt_sim_wakeup_on_incomming_toma_msg(m->k_consumers.low);
		mgmt_sim_wakeup_on_incomming_toma_msg(m->k_consumers.kal);
	}

	/* 2. Produce periodic messages to Toma */
	if ((m->hw.msg_count++ % 64) == 0) { 	/* Periodically inject hardwareConfiguration */
		m->hw.conf_version++;				// As if something in configuration changed
		mgmt_sim_send_msg_latest_hw_config();
	}

	if ((m->volume_msg_count++ % 15) == 0) {					/* Periodically send updateLeaderKeepaliveToken */
		const size_t capacity = 256;
		char *payload = malloc(capacity);
		const size_t len = make_msg_update_leader_keepalive_token(payload, capacity);
		sim_broker_topic_msg_produce(m->k_producers.l_vol, payload, len, false);
	}	// Todo: use make_msg_add_volume() here
}

/******************************************************************************/
/* Static helper functions                                                    */
/******************************************************************************/
static const char *mgmt_sim_fsm_state_name(enum mgmt_sim_fsm_state state) {
	switch (state) {
	case MGMT_FSM_WAITING_FOR_BOTH_OK:  return "waitingForBothOk";
	case MGMT_FSM_SENT_FORMAT_DRIVE:    return "sentFormatDrive";
	case MGMT_FSM_SAW_FORMATTING:       return "sawFormatting";
	case MGMT_FSM_DONE:                 return "done";
	default:                            return "unknown";
	}
}

static void __extract_disk_status_from_report_terget_msg(struct mm_json_elem *disks_array, struct mgmt_sim_disk_status *out) {
	int i;
	BUG_ON(!disks_array || disks_array->type != JSON_E_ARRAY || !out);
	for (i = 0; i < disks_array->array.len; i++) {
		struct mm_json_elem *disk_elem = disks_array->array.elements[i];
		const char *disk_id = json_get_dict_str(disk_elem, "diskID", NULL);
		BUG_ON(!disk_elem || (disk_elem->type != JSON_E_DICT) || !disk_id);
		if (strcmp(disk_id, out->disk_id) != 0)
			continue;

		nvmeibt_strlcpy(out->status, json_get_dict_str(disk_elem, "status", "unknown"), sizeof(out->status));
		out->format_request_counter = json_get_dict_num(disk_elem, "formatRequestCounter", -1);
		out->active_format_request_counter = json_get_dict_num(disk_elem, "activeFormatRequestCounter", -1);
		out->block_size = json_get_dict_num(disk_elem, "block_size", -1);
		out->metadata_size = json_get_dict_num(disk_elem, "metadata_size", -1);
		N_Tf(msim_disk, "disk=@STR status=@STR frc=@INT64_TD afrc=@INT64_TD, @UINT+@UINT[b]", out->disk_id, out->status, out->format_request_counter, out->active_format_request_counter, out->block_size, out->metadata_size);
		return;
	}
}

static void mgmt_sim_parse_report_target(struct mm_json_elem *root) {
	struct mm_json_elem *payload = json_get_dict_value(root,    "payload");
	struct mm_json_elem *node =    json_get_dict_value(payload, "node");
	struct mm_json_elem *disks =   json_get_dict_value(node,    "disks");
	struct mgmt_sim_state *m = g_mgmt_sim;

	m->boot_time = json_get_dict_num(node, "bootTime", 0);
	if (disks && (disks->type == JSON_E_ARRAY)) {
		__extract_disk_status_from_report_terget_msg(disks, &m->disk_002);
		__extract_disk_status_from_report_terget_msg(disks, &m->disk_003);
	}
	N_Tf(__AUTOID__, "reportTarget bootTime=@INT64_TD disk002=@STR disk003=@STR", m->boot_time, m->disk_002.status, m->disk_003.status);
}

/*
 * Run the format scenario state machine.
 * Transitions based on disk statuses extracted from reportTarget.
 * When a formatDrive needs to be sent, it's produced directly to the CMD topic.
 */
static void mgmt_sim_run_fsm(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	enum mgmt_sim_fsm_state prev_state;
	bool disk_002_ok;
	bool disk_003_ok;
	bool disk_003_formatting;
	bool disk_003_ok_with_expected_reported_format;
	BUG_ON(!m);
	#define FORMAT_REQUEST_COUNTER   303

	prev_state = m->fsm_state;
	disk_002_ok = (strcmp(m->disk_002.status, "Ok") == 0);
	disk_003_ok = (strcmp(m->disk_003.status, "Ok") == 0);
	disk_003_formatting = (strcmp(m->disk_003.status, "Formatting") == 0);
	disk_003_ok_with_expected_reported_format = disk_003_ok &&
		(m->disk_003.format_request_counter == FORMAT_REQUEST_COUNTER) &&
		(m->disk_003.active_format_request_counter == FORMAT_REQUEST_COUNTER) &&
		(m->disk_003.block_size == 4096) &&
		(m->disk_003.metadata_size == 8);

	switch (m->fsm_state) {
	case MGMT_FSM_WAITING_FOR_BOTH_OK:
		if (disk_002_ok && disk_003_ok) {		/* Both disks are Ok - send formatDrive */
			char *msg = malloc(1024);
			const size_t len = (size_t)make_msg_format_drive(msg, 1024, &m->disk_003, FORMAT_REQUEST_COUNTER, (unsigned long)m->boot_time);
			N_IMf(msim_fsm1, "both disks Ok, sending formatDrive bootTime=@INT64_TD", m->boot_time);
			sim_broker_topic_msg_produce(m->k_producers.cmd, msg, len, false);
			m->fsm_state = MGMT_FSM_SENT_FORMAT_DRIVE;
		}
		break;

	case MGMT_FSM_SENT_FORMAT_DRIVE:
		if (disk_003_formatting) {
			N_IMf(msim_fsm2, "disk003 now Formatting");
			m->fsm_state = MGMT_FSM_SAW_FORMATTING;
		} else if (disk_003_ok_with_expected_reported_format) {
			/* Might have missed the Formatting state - go directly to done */
			N_IMf(msim_fsm2b, "disk003 Ok with expected format (skipped Formatting) counter=@INT", FORMAT_REQUEST_COUNTER);
			m->fsm_state = MGMT_FSM_DONE;
		}
		break;

	case MGMT_FSM_SAW_FORMATTING:
		if (disk_003_ok_with_expected_reported_format) {
			N_IMf(msim_fsm3, "disk003 Ok with expected format counter=@INT", FORMAT_REQUEST_COUNTER);
			m->fsm_state = MGMT_FSM_DONE;
		}
		break;

	case MGMT_FSM_DONE:
		/* Already done - nothing to do */
		break;
	}

	if (m->fsm_state != prev_state) {
		N_IMf(msim_trans, "FSM transition @STR -> @STR", mgmt_sim_fsm_state_name(prev_state), mgmt_sim_fsm_state_name(m->fsm_state));
	}
}

const char *mgmt_sim_get_state_name(void) { return mgmt_sim_fsm_state_name(g_mgmt_sim->fsm_state); }
