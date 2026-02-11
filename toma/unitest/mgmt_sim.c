/*
 * mgmt_sim.c - Management simulator for Toma sandbox unit tests
 *
 * This module simulates the management server's Kafka message exchanges with Toma.
 */

#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS/library functions from this module - must be defined before any other includes

// Module interface header
#include "mgmt_sim.h"

// Sandbox internal headers
#include "sandbox_util.h"

// Toma headers
#include "nvmeibt_json_base.h"
#include "nvmeibt_debug.h"

// Standard library headers
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/*
 * Management simulation Kafka message formatting functions.
 * Each function formats a specific message type that the simulated management
 * server sends to Toma. Returns the number of bytes written to the buffer.
 */
#define MGMT_DB_UUID_JSON "\"dbUUID\":\"141d3140-c3c0-11f0-bc49-e391b6ca4c2b\""

/* Format scenario constants */
#define FORMAT_TARGET_DISK_ID    "NVMD_SN_003.1"
#define FORMAT_TARGET_UUID       "f39cebd1-c3c0-11f0-bc49-e391b6ca4c2b"
#define FORMAT_TARGET_VENDOR     5123
#define FORMAT_REQUEST_COUNTER   303

/*
 * State machine stats for the formatDrive scenario.
 */
 enum mgmt_sim_fsm_state {
	MGMT_FSM_WAITING_FOR_BOTH_OK, // waiting for both disks to report status="Ok"
	MGMT_FSM_SENT_FORMAT_DRIVE, // sent formatDrive message to Toma
	MGMT_FSM_SAW_FORMATTING, // disk reported status="Formatting"
	MGMT_FSM_DONE // test scenario complete - we got the updated disk format in Toma's reportTarget message
};

static int make_msg_update_leader_keepalive_token(char *buf, size_t capacity)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"updateLeaderKeepaliveToken\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"token\":1,\"keepaliveInterval\":5}}");
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

/* Format formatDrive message */
static int make_msg_format_drive(char *buf, size_t capacity, const char *disk_id,
				     const char *uuid, unsigned vendor,
				     unsigned format_request_counter, unsigned long boot_time)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"formatDrive\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"diskID\":\"%s\",\"uuid\":\"%s\",\"vendor\":%u"
		",\"formatType\":\"format_ec\",\"formatRequestCounter\":%u"
		",\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu"
		", " MGMT_DB_UUID_JSON "}}",
		disk_id, uuid, vendor, format_request_counter, boot_time);
}

/* Forward declarations */
static void mgmt_sim_parse_report_target(struct mm_json_elem *root);
static void mgmt_sim_run_fsm(void);
static const char *mgmt_sim_fsm_state_name(enum mgmt_sim_fsm_state state);

/* Per-disk status extracted from reportTarget */
struct mgmt_sim_disk_status {
	char disk_id[64];
	char status[32];
	int64_t format_request_counter;
	int64_t active_format_request_counter;
	int64_t block_size;
	int64_t metadata_size;
};

/* Management simulator state */
struct mgmt_sim_state {
	struct node_conf {
		const char *hostname;
		const char *uuid;
	} nodes[3], *live, *other;	// Cluster of 3 machines, 1 live followed by 2 simulated other tomas
	char *last_report_target_json; /* owned, NUL-terminated; NULL if not received */

	/* Per-consumer state for deterministic message sequencing */
	struct {
		int msg_count, conf_version;
	} hw;
	int cmd_msg_count;
	int target_msg_count;		// Ever increasing raft domain msg counter
	int volume_msg_count;

	/* Per-Producer state for deterministic message sequencing */
	int n_leader_keep_alives;

	/* State machine for formatDrive scenario */
	enum mgmt_sim_fsm_state fsm_state;
	int64_t boot_time;                      /* from reportTarget payload.node.bootTime */
	struct mgmt_sim_disk_status disk_002;   /* NVMD_SN_002.1 */
	struct mgmt_sim_disk_status disk_003;   /* NVMD_SN_003.1 */
	char *pending_format_drive_msg;         /* queued formatDrive message, or NULL */
	size_t pending_format_drive_len;
};

static struct mgmt_sim_state *g_mgmt_sim = NULL;

struct mgmt_sim_state *mgmt_sim_init(const char *live_toma_hostname)
{
	struct mgmt_sim_state *m = g_mgmt_sim = calloc(1, sizeof(*g_mgmt_sim));
	BUG_ON(!m || !live_toma_hostname);
	m->live =  &m->nodes[0];
	m->other = &m->nodes[1];
	   m->live->hostname = live_toma_hostname;
	m->other[0].hostname = "n37@nvidia.com";
	m->other[1].hostname = "n39@nvidia.com";
	m->live->uuid =    "cde269b0-0000-0000-0000-000000000000";
	m->other[0].uuid = "cde269b1-0000-0000-0000-000000000000";
	m->other[1].uuid = "cde269b2-0000-0000-0000-000000000000";

	/* Initialize state machine */
	m->fsm_state = MGMT_FSM_WAITING_FOR_BOTH_OK;
	m->boot_time = 0;
	nvmeibt_strlcpy(m->disk_002.disk_id, "NVMD_SN_002.1", sizeof(m->disk_002.disk_id));
	nvmeibt_strlcpy(m->disk_003.disk_id, "NVMD_SN_003.1", sizeof(m->disk_003.disk_id));
	m->hw.conf_version = 17;		// Start from some number
	N_Tf(msim_init, "mgmt_sim initialized cluster @INT machines, hw_conf_ver=@INT", (int)ARRAY_SIZE(m->nodes), m->hw.conf_version);
	return m;
}

static int make_msg_update_toma_keepalive_token(char *buf, size_t capacity) {
	const struct mgmt_sim_state *m = g_mgmt_sim;
	return snprintf(buf, capacity,
		"{\"messageType\":\"updateTomaKeepaliveToken\",\"messageTypeVersion\":1"
		",\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"1\",\"keepaliveInterval\":5}}",
		m->live->hostname);
}

static int make_msg_add_target(char *buf, size_t capacity, int queue_offset) {
	struct mgmt_sim_state *m = g_mgmt_sim;	// This Kafka queue is never purged. It has 3 messages for 3 targets in raft domain (offsets 0..2)
	struct node_conf *node = &m->nodes[queue_offset];
	BUG_ON((queue_offset < 0) || (queue_offset >= (int)ARRAY_SIZE(m->nodes)));
	++m->target_msg_count;					// Counter can get high, if leader changes and rereads the target kafka queue from beginning
	return snprintf(buf, capacity,			/* First message: addTarget (self as 1-machine raft domain), then the other 2 */
		"{\"messageType\":\"addTarget\",\"messageTypeVersion\":1,\"payload\":"
		"{\"nodeID\":\"%s\",\"uuid\":\"%s\",\"targetsInZone\":%d,\"targetUpdatesSequence\":%d}}",
		node->hostname, node->uuid, queue_offset, queue_offset);
	// Todo: Also test remove "deleteTarget" and add it back
}

static int make_msg_hardware_configuration(char *buf, size_t capacity) {
	const struct mgmt_sim_state *m = g_mgmt_sim;
	return snprintf(buf, capacity,
		"{\"messageType\":\"hardwareConfiguration\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"managementConfiguration\":{\"_id\":\"1\""
		",\"configurationVersion\":%d,\"leaderToken\":1,\"kafkaMessageSequence\":%d,\"raftTerm\":9"
		",\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},"
		"\"targets\":["
			"{\"_id\":\"nvme37.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"%s\",\"blocks\":2000,\"block_size\":4096"
					",\"activeFormatRequestCounter\":1,\"vendorID\":5122"
					",\"uuid\":\"f39cebd0-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"%s\",\"blocks\":2000,\"block_size\":4096"
					",\"activeFormatRequestCounter\":1,\"vendorID\":5123"
					",\"uuid\":\"%s\",\"version\":7,\"isOutOfService\":false}],"
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
		m->live->hostname, m->live->uuid, m->disk_002.disk_id, m->disk_003.disk_id, FORMAT_TARGET_UUID,
		m->other[0].hostname, m->other[0].uuid,
		m->other[1].hostname, m->other[1].uuid);
}

char *mgmt_sim_next_kafka_payload(const char *consumer_name, int queue_offset, size_t *out_len)
{
	char *payload = NULL;
	size_t len = 0;
	BUG_ON(!g_mgmt_sim || !consumer_name || !out_len || (queue_offset < 0));

	if (strncmp(consumer_name, "HW", 2) == 0) {
		if ((g_mgmt_sim->hw.msg_count++ % 64) == 0) { 	/* Periodically inject hardwareConfiguration */
			const size_t capacity = 4096;
			payload = malloc(capacity);
			BUG_ON(!payload);
			g_mgmt_sim->hw.conf_version++;				// As if something in configuration changed
			len = make_msg_hardware_configuration(payload, capacity);
		}
	} else if (strncmp(consumer_name, "CMD", 3) == 0) {
		/* Check for pending formatDrive message first */
		if (g_mgmt_sim->pending_format_drive_msg != NULL) {
			payload = g_mgmt_sim->pending_format_drive_msg;
			len = g_mgmt_sim->pending_format_drive_len;
			g_mgmt_sim->pending_format_drive_msg = NULL;
			g_mgmt_sim->pending_format_drive_len = 0;
			N_IMf(msim_cmd, "delivering formatDrive message len=@INT", (int)len);
		} else if (g_mgmt_sim->cmd_msg_count == 0) {
			/* First command: updateTomaKeepaliveToken (zone) */
			const size_t capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			N_Tf(__AUTOID__, "consumer[@STR] << msg=updateZone", consumer_name);
			len = make_msg_update_toma_keepalive_token(payload, capacity);
			g_mgmt_sim->cmd_msg_count++;
		}
	} else if (strstr(consumer_name, "incrementalTarget") != NULL) {		// Leader raft domain
		if (queue_offset < 8) {	// Kafka offsets are [7,8,9] for the 3 messages
			const size_t capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			N_Tf(__AUTOID__, "consumer[@STR] << msg=addTarget(koffset=@INT)", consumer_name, queue_offset);
			len = make_msg_add_target(payload, capacity, queue_offset-7);
		}
	} else if (strstr(consumer_name, "incrementalUpdates") != NULL) {		// Leader volumes updates domain
		if ((g_mgmt_sim->volume_msg_count++ % 15) == 0) {					/* Periodically send updateLeaderKeepaliveToken */
			const size_t capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			N_Tf(__AUTOID__, "consumer[@STR] << msg=updateLeaderKeepaliveToken", consumer_name);
			len = make_msg_update_leader_keepalive_token(payload, capacity);
		}// Todo: use make_msg_add_volume() here
	} else {
		BUG_ON(true);
	}

	*out_len = len;
	return payload;
}

void mgmt_sim_on_toma_produced(enum sim_topic_type_toma_to_mgmt type, const void *payload, size_t len) {
	struct mm_json_elem *root;
	const char *message_type;

	BUG_ON(!g_mgmt_sim || !payload || len == 0 || (type == KTOPIC_TYPE_T2M_UNKNOWN));
	// N_Tf(__AUTOID__, "[@CHAR] @INT[b] |@STR|", type, len, (const char*)payload);
	if (type == KTOPIC_TYPE_T2M_LOW) {
		// {"originType":"TOMA","messageType":"driveZeroingProgress","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":27,"leaderToken":null,"payload":{"zeroWriteCounter":303100870554,"nZeroedBlks":1953536,"diskUUID":"de2eadb0-e972-11f0-8e2a-434e55e1d7f7","node_id":"nvme37.nvidia.com"}}
		// {"originType":"TOMA","messageType":"updateDiskSegmentsDirtyBits","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":97,"leaderToken":null,"payload":{"segmentsDirtyBitsUpdate":[{"pRaidMinorVersion":2,"pRaidMajorVersion":259,"segmentID":"c1105e50-e97b-11f0-995c-3792ee0db955","pRaidUUID":"c1103742-e97b-11f0-995c-3792ee0db955","remainingDirtyBits":67932,"reappearingCounter":3}]}}
		return;	// Todo: handle drive zeroing reports here
	}

	root = parse_json_txt_into_kv_tree((const char *)payload, (int)len);
	message_type = json_get_dict_str(root, "messageType", NULL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type );

	if (type == KTOPIC_TYPE_T2M_KEEPALIVE) {
		/* Todo: Currently Only handle leader keep alive */
		if (strcmp(message_type, "leaderKeepalive") == 0) {
			// {"originType":"TOMA","messageType":"leaderKeepalive","messageTypeVersion":1,"hostname":"nvme39.nvidia.com","tomaToken":2,"messageSequence":24312,"leaderToken":1,"keepaliveInterval":5,"payload":{"raftTerm":10,"zone":"1","featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":""}}
			g_mgmt_sim->n_leader_keep_alives++;
		} else if (strcmp(message_type, "keepalive") == 0) {
			// {"originType":"TOMA","messageType":"keepalive","messageTypeVersion":2,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":11831,"leaderToken":null,"keepaliveInterval":5,"payload":{"zone":"1","leaderUUID":"nvme39.nvidia.com","bootTime":1767279770145,"featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":"","rebuildStats":{"nRunningDirtyRebuild":0,"nPendingDirtyRebuild":0,"nRunningStaleRebuild":0,"nPendingStaleRebuild":6,"nRunningTxidRebuild":0,"nPendingTxidRebuild":0,"nRunningColdRecovery":0,"nPendingColdRecovery":0,"nRunningJGCRebuild":0,"nPendingJGCRebuild":0,"nRunningScrubbing":0,"nPendingScrubbing":3}}}
		}
	} else {	// KTOPIC_TYPE_T2M_PRIORITY
		/* Only handle reportTarget produced by Toma */
		if (strcmp(message_type, "reportTarget") == 0) {
			// {"originType":"TOMA","messageType":"reportTarget","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":24,"leaderToken":null,"payload":{"node":{"zone":"1","bootTime":1767535819492,"cpu_temp":"30.0","version":"3.3.0-1340","buildNumber":"","reportID":25,"branch":"master","commit":"18af9e759c828cc1d3776bfdd6cb68ade54ef738","configProfile":{"id":"569407c0-e976-11f0-b6cb-871ca30885b4","name":"Cluster Default","version":"1"},"node_status":"1","node_id":"nvme37.nvidia.com","targetUpdatesSequence":4,"cpu_load":"0.0","disks":[{"diskID":"S3HCNX0JC01918.1","disk_version":0,"blocks":1562500000,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01918","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"1_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5f","Power_On_Hours":"0x10049","Unsafe_Shutdowns":"0x56","Media_Errors":"0x2","Number_of_Error_Information_Log_Entries":"0x1a5d","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":25439882564,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700220.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700220","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xb4","Power_On_Hours":"0xfb7f","Unsafe_Shutdowns":"0x94","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x5980","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4546392587,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0JC01904.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01904","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"2_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5e","Power_On_Hours":"0x10046","Unsafe_Shutdowns":"0x54","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x2beb","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":49694267592,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700164.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700164","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x6e","Power_On_Hours":"0x1062c","Unsafe_Shutdowns":"0x5c","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x46","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4675551222,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0K600397.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0K600397","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xca","Power_On_Hours":"0xe767","Unsafe_Shutdowns":"0xb5","Media_Errors":"0x3","Number_of_Error_Information_Log_Entries":"0x165c","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":29489346822,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"disID":"S4C9NF0M500226.1","disk_version":0,"blocks":3125627568,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S4C9NF0M500226","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL1T6HAJQ-00005","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"4_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x56","Power_On_Hours":"0xda5d","Unsafe_Shutdowns":"0x49","Media_Errors":"0x57","Number_of_Error_Information_Log_Entries":"0x1c5a","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":303100870572,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S665NE0R702075.1","disk_version":0,"blocks":1875385008,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S665NE0R702075","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZ1L2960HCJR-00A07","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"3_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x70","Power_On_Hours":"0x817d","Unsafe_Shutdowns":"0x53","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x0","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":10392940702,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0}],"nics":[{"nicID":"0x0000000000000000bae924fffee5cfd8","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0125","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_2"},{"nicID":"0x0000000000000000bae924fffee5cfd9","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0225","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_3"}]}}}
			mgmt_sim_parse_report_target(root);
			mgmt_sim_run_fsm();
		} else  if (strcmp(message_type, "updatePRaidReport") == 0) {
		 	// {"originType":"TOMA","messageType":"updatePRaidReport","messageTypeVersion":1,"hostname":"nvme39.nvidia.com","tomaToken":2,"messageSequence":85,"leaderToken":1,"payload":{"pRaidsUpdate":[{"uuid":"b60b04b1-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60b04b0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60b2bc0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]},{"uuid":"b60ab692-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60ab691-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60adda0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]}]}}
		} else if (strcmp(message_type, "segmentZeroingProgress") == 0) {
			// {"originType":"TOMA","messageType":"segmentZeroingProgress","messageTypeVersion":1,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":335,"leaderToken":null,"payload":{"praidVersion":"258.0","segmentUUID":"98e46d20-ea22-11f0-bad8-af65dd8e6ead","pRaidUUID":"98e44612-ea22-11f0-bad8-af65dd8e6ead","nZeroedBlks":262144}}
		}
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

void mgmt_sim_verify_at_end(void)
{
	const char *state = mgmt_sim_get_state_name();
	const struct mgmt_sim_disk_status *d3 = &g_mgmt_sim->disk_003;
	bool done = mgmt_sim_is_done();

	BUG_ON((g_mgmt_sim->volume_msg_count < 0) || (g_mgmt_sim->n_leader_keep_alives < 0));
	if (!done) {
		SANDBOX_PRINT(
			"failed: format FSM did not reach done, state=%s disk=%s status=%s frc=%lld afrc=%lld bs=%lld ms=%lld\n",
			state, d3->disk_id, d3->status, (long long)d3->format_request_counter,
			(long long)d3->active_format_request_counter, (long long)d3->block_size,
			(long long)d3->metadata_size);
		BUG_ON(true);
	}
}

bool mgmt_sim_is_done(void)
{
	if (!g_mgmt_sim)
		return false;
	return g_mgmt_sim->fsm_state == MGMT_FSM_DONE;
}

const char *mgmt_sim_get_state_name(void)
{
	if (!g_mgmt_sim)
		return "uninitialized";
	return mgmt_sim_fsm_state_name(g_mgmt_sim->fsm_state);
}

void mgmt_sim_destroy(void)
{
	if (!g_mgmt_sim)
		return;

	free(g_mgmt_sim->last_report_target_json);
	free(g_mgmt_sim->pending_format_drive_msg);
	free(g_mgmt_sim);
	g_mgmt_sim = NULL;
}

/******************************************************************************/
/* Static helper functions                                                    */
/******************************************************************************/

static const char *mgmt_sim_fsm_state_name(enum mgmt_sim_fsm_state state)
{
	switch (state) {
	case MGMT_FSM_WAITING_FOR_BOTH_OK:  return "waitingForBothOk";
	case MGMT_FSM_SENT_FORMAT_DRIVE:    return "sentFormatDrive";
	case MGMT_FSM_SAW_FORMATTING:       return "sawFormatting";
	case MGMT_FSM_DONE:                 return "done";
	default:                            return "unknown";
	}
}

/*
 * Extract disk status from reportTarget JSON for a specific disk.
 * Searches payload.node.disks[] array for the matching diskID.
 */
static void mgmt_sim_extract_disk_status(struct mm_json_elem *disks_array,
					 const char *target_disk_id,
					 struct mgmt_sim_disk_status *out)
{
	int i;
	struct mm_json_elem *disk_elem;
	const char *disk_id;

	BUG_ON(!disks_array || disks_array->type != JSON_E_ARRAY || !out);

	for (i = 0; i < disks_array->array.len; i++) {
		disk_elem = disks_array->array.elements[i];
		if (!disk_elem || disk_elem->type != JSON_E_DICT)
			continue;

		disk_id = json_get_dict_str(disk_elem, "diskID", NULL);
		if (!disk_id || strcmp(disk_id, target_disk_id) != 0)
			continue;

		/* Found matching disk */
		nvmeibt_strlcpy(out->status,
				json_get_dict_str(disk_elem, "status", "unknown"),
				sizeof(out->status));
		out->format_request_counter = json_get_dict_num(disk_elem, "formatRequestCounter", -1);
		out->active_format_request_counter = json_get_dict_num(disk_elem, "activeFormatRequestCounter", -1);
		out->block_size = json_get_dict_num(disk_elem, "block_size", -1);
		out->metadata_size = json_get_dict_num(disk_elem, "metadata_size", -1);

		N_Tf(msim_disk, "disk=@STR status=@STR frc=@INT64_TD afrc=@INT64_TD bs=@INT64_TD ms=@INT64_TD",
		     target_disk_id, out->status,
		     out->format_request_counter, out->active_format_request_counter,
		     out->block_size, out->metadata_size);
		return;
	}
}

/*
 * Parse reportTarget JSON and extract relevant information.
 * Updates g_mgmt_sim with bootTime and disk statuses.
 */
static void mgmt_sim_parse_report_target(struct mm_json_elem *root)
{
	struct mm_json_elem *payload;
	struct mm_json_elem *node;
	struct mm_json_elem *disks;

	BUG_ON(!g_mgmt_sim || !root);

	if (root->type != JSON_E_DICT) {
		N_Wf(msim_parse, "reportTarget root is not a dict");
		return;
	}

	/* Navigate: payload.node */
	payload = json_get_dict_value(root, "payload");
	if (!payload || payload->type != JSON_E_DICT) {
		N_Wf(msim_nopl, "reportTarget missing payload");
		return;
	}

	node = json_get_dict_value(payload, "node");
	if (!node || node->type != JSON_E_DICT) {
		N_Wf(msim_nonode, "reportTarget missing payload.node");
		return;
	}

	/* Extract bootTime */
	g_mgmt_sim->boot_time = json_get_dict_num(node, "bootTime", 0);

	/* Extract disk statuses */
	disks = json_get_dict_value(node, "disks");
	if (disks && disks->type == JSON_E_ARRAY) {
		mgmt_sim_extract_disk_status(disks, "NVMD_SN_002.1", &g_mgmt_sim->disk_002);
		mgmt_sim_extract_disk_status(disks, "NVMD_SN_003.1", &g_mgmt_sim->disk_003);
	}

	N_Tf(msim_rt, "reportTarget bootTime=@INT64_TD disk002=@STR disk003=@STR",
	     g_mgmt_sim->boot_time, g_mgmt_sim->disk_002.status, g_mgmt_sim->disk_003.status);
}

/*
 * Run the format scenario state machine.
 * Transitions based on disk statuses extracted from reportTarget.
 */
static void mgmt_sim_run_fsm(void)
{
	enum mgmt_sim_fsm_state prev_state;
	bool disk_002_ok;
	bool disk_003_ok;
	bool disk_003_formatting;
	bool disk_003_ok_with_expected_reported_format;
	char *msg;
	size_t msg_len;

	BUG_ON(!g_mgmt_sim);

	prev_state = g_mgmt_sim->fsm_state;
	disk_002_ok = (strcmp(g_mgmt_sim->disk_002.status, "Ok") == 0);
	disk_003_ok = (strcmp(g_mgmt_sim->disk_003.status, "Ok") == 0);
	disk_003_formatting = (strcmp(g_mgmt_sim->disk_003.status, "Formatting") == 0);
	disk_003_ok_with_expected_reported_format = disk_003_ok &&
		(g_mgmt_sim->disk_003.format_request_counter == FORMAT_REQUEST_COUNTER) &&
		(g_mgmt_sim->disk_003.active_format_request_counter == FORMAT_REQUEST_COUNTER) &&
		(g_mgmt_sim->disk_003.block_size == 4096) &&
		(g_mgmt_sim->disk_003.metadata_size == 8);

	switch (g_mgmt_sim->fsm_state) {
	case MGMT_FSM_WAITING_FOR_BOTH_OK:
		if (disk_002_ok && disk_003_ok) {
			/* Both disks are Ok - send formatDrive */
			N_IMf(msim_fsm1, "both disks Ok, sending formatDrive bootTime=@INT64_TD", g_mgmt_sim->boot_time);

			msg = malloc(1024);
			BUG_ON(!msg);
			msg_len = (size_t)make_msg_format_drive(msg, 1024,
				FORMAT_TARGET_DISK_ID, FORMAT_TARGET_UUID,
				FORMAT_TARGET_VENDOR, FORMAT_REQUEST_COUNTER,
				(unsigned long)g_mgmt_sim->boot_time);
			g_mgmt_sim->pending_format_drive_msg = msg;
			g_mgmt_sim->pending_format_drive_len = msg_len;
			g_mgmt_sim->fsm_state = MGMT_FSM_SENT_FORMAT_DRIVE;
		}
		break;

	case MGMT_FSM_SENT_FORMAT_DRIVE:
		if (disk_003_formatting) {
			N_IMf(msim_fsm2, "disk003 now Formatting");
			g_mgmt_sim->fsm_state = MGMT_FSM_SAW_FORMATTING;
		} else if (disk_003_ok_with_expected_reported_format) {
			/* Might have missed the Formatting state - go directly to done */
			N_IMf(msim_fsm2b, "disk003 Ok with expected format (skipped Formatting) counter=@INT",
			     FORMAT_REQUEST_COUNTER);
			g_mgmt_sim->fsm_state = MGMT_FSM_DONE;
		}
		break;

	case MGMT_FSM_SAW_FORMATTING:
		if (disk_003_ok_with_expected_reported_format) {
			N_IMf(msim_fsm3, "disk003 Ok with expected format counter=@INT", FORMAT_REQUEST_COUNTER);
			g_mgmt_sim->fsm_state = MGMT_FSM_DONE;
		}
		break;

	case MGMT_FSM_DONE:
		/* Already done - nothing to do */
		break;
	}

	if (g_mgmt_sim->fsm_state != prev_state) {
		N_IMf(msim_trans, "FSM transition @STR -> @STR",
		     mgmt_sim_fsm_state_name(prev_state),
		     mgmt_sim_fsm_state_name(g_mgmt_sim->fsm_state));
	}
}
