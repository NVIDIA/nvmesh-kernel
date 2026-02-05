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

/* Format updateLeaderKeepaliveToken message */
static int make_msg_update_leader_keepalive_token(char *buf, size_t capacity)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"updateLeaderKeepaliveToken\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"token\":1,\"keepaliveInterval\":5}}");
}

/* Format updateTomaKeepaliveToken message */
static int make_msg_update_toma_keepalive_token(char *buf, size_t capacity, const char *hostname)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"updateTomaKeepaliveToken\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"1\",\"keepaliveInterval\":5}}",
		hostname);
}

/* Format addTarget message */
static int make_msg_add_target(char *buf, size_t capacity, const char *hostname)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"addTarget\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"nodeID\":\"%s\""
		",\"uuid\":\"e8c70c10-db79-11f0-8f35-cb935b7ef6ae\""
		",\"targetsInZone\":0,\"targetUpdatesSequence\":1}}",
		hostname);
}

/*
 * Format hardwareConfiguration message.
 * Simulated disks: NVMD_SN_002.1 (vendor 5122), NVMD_SN_003.1 (vendor 5123) with 2000 blocks each.
 */
static int make_msg_hardware_configuration(char *buf, size_t capacity, int kafka_msg_seq, const char *hostname)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"hardwareConfiguration\""
		",\"messageTypeVersion\":1"
		",\"payload\":{\"managementConfiguration\":{\"_id\":\"1\""
		",\"configurationVersion\":17,\"leaderToken\":1"
		",\"kafkaMessageSequence\":%d,\"raftTerm\":9"
		",\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},"
		"\"targets\":["
		"{\"_id\":\"nvme38.mlnx\",\"node_id\":\"%s\""
		",\"uuid\":\"cde269b0-c3c0-11f0-bc49-e391b6ca4c2b\","
		"\"disks\":["
		"{\"diskID\":\"NVMD_SN_002.1\",\"blocks\":2000,\"block_size\":4096"
		",\"activeFormatRequestCounter\":1,\"vendorID\":5122"
		",\"uuid\":\"f39cebd0-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
		"{\"diskID\":\"NVMD_SN_003.1\",\"blocks\":2000,\"block_size\":4096"
		",\"activeFormatRequestCounter\":1,\"vendorID\":5123"
		",\"uuid\":\"f39cebd1-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false}],"
		"\"nics\":["
		"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\""
		",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1"
		",\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
		"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\""
		",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1"
		",\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
		"{\"_id\":\"nvme39.mlnx\",\"node_id\":\"n39@google.com\""
		",\"uuid\":\"cde269b1-c3c0-11f0-bc49-e391b6ca4c2b\","
		"\"disks\":["
		"{\"diskID\":\"D0_n39\",\"blocks\":195353046,\"block_size\":4096"
		",\"activeFormatRequestCounter\":1,\"vendorID\":5197"
		",\"uuid\":\"f39cebd2-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
		"{\"diskID\":\"D1_n39\",\"blocks\":195353046,\"block_size\":1024"
		",\"activeFormatRequestCounter\":0,\"vendorID\":3333"
		",\"uuid\":\"f39cebd3-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":1,\"isOutOfService\":false}],"
		"\"nics\":["
		"{\"nicID\":\"0x0000000000000000bae924fffee5e008\",\"protocol\":\"RoCE\""
		",\"guid\":\"0x00000000000000000000ffff0a0b0126\",\"pkey\":65535,\"version\":1"
		",\"uuid\":\"cff4cef2-c3c0-11f0-bc49-e391b6ca4c2b\"},"
		"{\"nicID\":\"0x0000000000000000bae924fffee5e009\",\"protocol\":\"RoCE\""
		",\"guid\":\"0x00000000000000000000ffff0a0b0226\",\"pkey\":65535,\"version\":1"
		",\"uuid\":\"cff4ce12-c3c0-11f0-bc49-e391b6ca4c2b\"}]}"
		"]}}",
		kafka_msg_seq, hostname);
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
static void mgmt_sim_parse_report_target(const char *json, size_t len);
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
	const char *hostname;
	char *last_report_target_json; /* owned, NUL-terminated; NULL if not received */

	/* Per-consumer state for deterministic message sequencing */
	int hw_msg_count;
	int cmd_msg_count;
	int target_msg_count;

	/* State machine for formatDrive scenario */
	enum mgmt_sim_fsm_state fsm_state;
	int64_t boot_time;                      /* from reportTarget payload.node.bootTime */
	struct mgmt_sim_disk_status disk_002;   /* NVMD_SN_002.1 */
	struct mgmt_sim_disk_status disk_003;   /* NVMD_SN_003.1 */
	char *pending_format_drive_msg;         /* queued formatDrive message, or NULL */
	size_t pending_format_drive_len;
};

static struct mgmt_sim_state *g_mgmt_sim = NULL;

struct mgmt_sim_state *mgmt_sim_init(const char *my_hostname)
{
	g_mgmt_sim = calloc(1, sizeof(*g_mgmt_sim));
	BUG_ON(!g_mgmt_sim || !my_hostname);
	g_mgmt_sim->hostname = my_hostname;

	/* Initialize state machine */
	g_mgmt_sim->fsm_state = MGMT_FSM_WAITING_FOR_BOTH_OK;
	g_mgmt_sim->boot_time = 0;
	nvmeibt_strlcpy(g_mgmt_sim->disk_002.disk_id, "NVMD_SN_002.1", sizeof(g_mgmt_sim->disk_002.disk_id));
	nvmeibt_strlcpy(g_mgmt_sim->disk_003.disk_id, "NVMD_SN_003.1", sizeof(g_mgmt_sim->disk_003.disk_id));

	N_Tf(msim_init, "mgmt_sim initialized hostname=@STR", g_mgmt_sim->hostname);
	return g_mgmt_sim;
}

char *mgmt_sim_next_kafka_payload(const char *consumer_name, size_t *out_len)
{
	char *payload = NULL;
	size_t len = 0;
	size_t capacity;

	BUG_ON(!g_mgmt_sim || !consumer_name || !out_len);

	/* HW consumer - hardware configuration messages */
	if (strncmp(consumer_name, "HW", 2) == 0) {
		if (g_mgmt_sim->hw_msg_count == 0) {
			/* First message: updateTomaKeepaliveToken */
			capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			len = (size_t)make_msg_update_toma_keepalive_token(
				payload, capacity, g_mgmt_sim->hostname);
			g_mgmt_sim->hw_msg_count++;
		} else if ((g_mgmt_sim->hw_msg_count++ % 4) == 0) {
			/* Periodically inject hardwareConfiguration */
			capacity = 4096;
			payload = malloc(capacity);
			BUG_ON(!payload);
			len = (size_t)make_msg_hardware_configuration(
				payload, capacity, g_mgmt_sim->hw_msg_count, g_mgmt_sim->hostname);
		}
	}
	/* CMD consumer - command messages */
	else if (strncmp(consumer_name, "CMD", 3) == 0) {
		/* Check for pending formatDrive message first */
		if (g_mgmt_sim->pending_format_drive_msg != NULL) {
			payload = g_mgmt_sim->pending_format_drive_msg;
			len = g_mgmt_sim->pending_format_drive_len;
			g_mgmt_sim->pending_format_drive_msg = NULL;
			g_mgmt_sim->pending_format_drive_len = 0;
			N_IMf(msim_cmd, "delivering formatDrive message len=@INT", (int)len);
		} else if (g_mgmt_sim->cmd_msg_count == 0) {
			/* First command: updateTomaKeepaliveToken (zone) */
			capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			len = (size_t)make_msg_update_toma_keepalive_token(
				payload, capacity, g_mgmt_sim->hostname);
			g_mgmt_sim->cmd_msg_count++;
		} else if ((g_mgmt_sim->cmd_msg_count++ % 3) == 0) {
			/* Periodically send updateLeaderKeepaliveToken */
			capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			len = (size_t)make_msg_update_leader_keepalive_token(payload, capacity);
		}
	}
	/* Target consumer (not incrementalTarget) - addTarget messages */
	else if (strstr(consumer_name, "incrementalTarget") == NULL) {
		if (g_mgmt_sim->target_msg_count == 0) {
			/* First message: addTarget (self as 1-machine raft domain) */
			capacity = 256;
			payload = malloc(capacity);
			BUG_ON(!payload);
			len = (size_t)make_msg_add_target(payload, capacity, g_mgmt_sim->hostname);
			g_mgmt_sim->target_msg_count++;
		}
	}

	*out_len = len;
	return payload;
}

void mgmt_sim_on_toma_produced(const void *payload, size_t len)
{
	const char *m_type;
	bool is_report_target;

	BUG_ON(!g_mgmt_sim || !payload || len == 0);

	/* Check if this is a reportTarget message */
	m_type = strstr((const char *)payload, "\"messageType\":");
	if (!m_type)
		return;

	is_report_target = (strncmp(m_type, "\"messageType\": \"reportTarget\"", 28) == 0);
	if (is_report_target) {
		free(g_mgmt_sim->last_report_target_json);
		g_mgmt_sim->last_report_target_json = strndup((const char *)payload, len);

		/* Parse the reportTarget and run state machine */
		mgmt_sim_parse_report_target(g_mgmt_sim->last_report_target_json, len);
		mgmt_sim_run_fsm();
	}
}

const char *mgmt_sim_get_last_report_target(void)
{
	if (!g_mgmt_sim)
		return NULL;
	return g_mgmt_sim->last_report_target_json;
}

void mgmt_sim_verify_at_end(void)
{
	const char *state;
	const struct mgmt_sim_disk_status *d3;
	bool done;

	BUG_ON(!g_mgmt_sim);
	state = mgmt_sim_get_state_name();
	d3 = &g_mgmt_sim->disk_003;
	done = mgmt_sim_is_done();
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
static void mgmt_sim_parse_report_target(const char *json, size_t len)
{
	struct mm_json_elem *root;
	struct mm_json_elem *payload;
	struct mm_json_elem *node;
	struct mm_json_elem *disks;

	BUG_ON(!g_mgmt_sim || !json);

	root = parse_json_txt_into_kv_tree(json, (int)len);
	if (!root) {
		N_Wf(msim_parse, "failed to parse reportTarget JSON");
		return;
	}

	/* Navigate: payload.node */
	payload = json_get_dict_value(root, "payload");
	if (!payload || payload->type != JSON_E_DICT) {
		N_Wf(msim_nopl, "reportTarget missing payload");
		goto cleanup;
	}

	node = json_get_dict_value(payload, "node");
	if (!node || node->type != JSON_E_DICT) {
		N_Wf(msim_nonode, "reportTarget missing payload.node");
		goto cleanup;
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

cleanup:
	nvmeibt_mm_json_free_kv_tree(root);
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
