#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS/library functions from this module - must be defined before any other includes

/*
 * mgmt_sim.c - Management simulator for Toma sandbox unit tests
 *
 * This module simulates the management server's Kafka message exchanges with Toma.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "mgmt_sim.h"

/*
 * Management simulation Kafka message templates.
 * These are the messages that the simulated management server sends to Toma.
 */
#define MGMT_DB_UUID_JSON "\"dbUUID\":\"141d3140-c3c0-11f0-bc49-e391b6ca4c2b\""

static const char *mgmt_simu_kafka_msg_templates[] = {
	/* [0] updateLeaderKeepaliveToken */
	"{\"messageType\":\"updateLeaderKeepaliveToken\",\"messageTypeVersion\":1,\"payload\":{\"token\":1,\"keepaliveInterval\":5}}",

	/* [1] updateTomaKeepaliveToken - %s = hostname */
	"{\"messageType\":\"updateTomaKeepaliveToken\""
	",\"messageTypeVersion\":1,\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"1\",\"keepaliveInterval\":5}}",

	/* [2] addTarget - %s = hostname */
	"{\"messageType\":\"addTarget\",\"messageTypeVersion\":1,\"payload\":{\"nodeID\":\"%s\",\"uuid\":\"e8c70c10-db79-11f0-8f35-cb935b7ef6ae\",\"targetsInZone\":0,\"targetUpdatesSequence\":1}}",

	/* [3] hardwareConfiguration - %d = kafkaMessageSequence, %s = hostname
	 * Simulated disks: NVMD_SN_002.1 (vendor 5122), NVMD_SN_003.1 (vendor 5123) with 2000 blocks each
	 */
	"{\"messageType\":\"hardwareConfiguration\"   "
	",\"messageTypeVersion\":1,\"payload\":{\"managementConfiguration\":{\"_id\":\"1\",\"configurationVersion\":17,\"leaderToken\":1,\"kafkaMessageSequence\""
	":%d,\"raftTerm\":9,\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},"
	"\"targets\":["
	"{\"_id\":\"nvme38.mlnx\",\"node_id\":\"%s\",\"uuid\":\"cde269b0-c3c0-11f0-bc49-e391b6ca4c2b\","
	"\"disks\":["
	"{\"diskID\":\"NVMD_SN_002.1\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5122,\"uuid\":\"f39cebd0-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
	"{\"diskID\":\"NVMD_SN_003.1\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5123,\"uuid\":\"f39cebd1-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false}],"
	"\"nics\":["
	"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
	"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
	"{\"_id\":\"nvme39.mlnx\",\"node_id\":\"n39@google.com\",\"uuid\":\"cde269b1-c3c0-11f0-bc49-e391b6ca4c2b\","
	"\"disks\":["
	"{\"diskID\":\"D0_n39\",\"blocks\":195353046,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5197,\"uuid\":\"f39cebd2-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":7,\"isOutOfService\":false},"
	"{\"diskID\":\"D1_n39\",\"blocks\":195353046,\"block_size\":1024,\"activeFormatRequestCounter\":0,\"vendorID\":3333,\"uuid\":\"f39cebd3-c3c0-11f0-bc49-e391b6ca4c2b\",\"version\":1,\"isOutOfService\":false}],"
	"\"nics\":["
	"{\"nicID\":\"0x0000000000000000bae924fffee5e008\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0b0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef2-c3c0-11f0-bc49-e391b6ca4c2b\"},"
	"{\"nicID\":\"0x0000000000000000bae924fffee5e009\",\"protocol\":\"RoCE\",\"guid\":\"0x00000000000000000000ffff0a0b0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce12-c3c0-11f0-bc49-e391b6ca4c2b\"}]}"
	"]}}",

	/* [4] addVolume */
	"{\"messageType\":\"addVolume\",\"messageTypeVersion\":1,\"payload\":{\"_id\":\"V1\",\"uuid\":\"f1b17590-c52b-11f0-bc49-e391b6ca4c2b\","
	"\"version\":1,\"name\":\"V1\",\"blockSize\":4096,\"lockServer\":{\"maxNOwners\":1,\"type\":4,\"locksetShift\":-1},\"blocks\":4882432,\"RAIDLevel\":\"Striped RAID-0\",\"numberOfMirrors\":0,\"stripeSize\":32,\"stripeWidth\":2,\"status\":\"unavailable\","
	"\"action\":\"initializing\",\"relativeRebuildPriority\":10,\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null,\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,"
	"\"chunks\":["
	"{\"uuid\":\"09c2f550-c52c-11f0-bc49-e391b6ca4c2b\",\"vlbs\":0,\"vlbe\":4882431,\"pRaids\":["
	"{\"uuid\":\"09c2f552-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false,\"stripeIndex\":0,\"zone\":\"1\",\"diskSegments\":["
	"{\"uuid\":\"09c2f551-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":26900224,\"lbe\":29341439,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"f3a2b830-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
	"{\"uuid\":\"09c34371-c52c-11f0-bc49-e391b6ca4c2b\",\"activated\":false,\"stripeIndex\":1,\"zone\":\"1\",\"diskSegments\":["
	"{\"uuid\":\"09c34370-c52c-11f0-bc49-e391b6ca4c2b\",\"lbs\":1509632,\"lbe\":3950847,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"f3a24300-c3c0-11f0-bc49-e391b6ca4c2b\"}"
	"]}]}]}}",

	/* [5] formatDrive - %s = diskID, %s = uuid, %u = vendor, %u = formatRequestCounter, %lu = bootTime */
	"{\"messageType\":\"formatDrive\",\"messageTypeVersion\":1,\"payload\":{\"diskID\":\"%s\",\"uuid\":\"%s\",\"vendor\":%u,\"formatType\":\"format_ec\",\"formatRequestCounter\":%u,\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu, " MGMT_DB_UUID_JSON
	"}}",
};

/* Management simulator state */
struct mgmt_sim_state {
	char hostname[64];
	char *last_report_target_json; /* owned, NUL-terminated; NULL if not received */

	/* Per-consumer state for deterministic message sequencing */
	int hw_msg_count;
	int cmd_msg_count;
	int target_msg_count;
};

static struct mgmt_sim_state *g_mgmt_sim = NULL;

void mgmt_sim_init(const char *my_hostname)
{
	g_mgmt_sim = calloc(1, sizeof(*g_mgmt_sim));
	if (!g_mgmt_sim)
		return;

	if (my_hostname) {
		strncpy(g_mgmt_sim->hostname, my_hostname, sizeof(g_mgmt_sim->hostname) - 1);
		g_mgmt_sim->hostname[sizeof(g_mgmt_sim->hostname) - 1] = '\0';
	}
}

char *mgmt_sim_next_kafka_payload(const char *consumer_name, size_t *out_len)
{
	char *payload = NULL;
	size_t len = 0;

	if (!g_mgmt_sim || !consumer_name || !out_len) {
		if (out_len)
			*out_len = 0;
		return NULL;
	}

	/* HW consumer - hardware configuration messages */
	if (strncmp(consumer_name, "HW", 2) == 0) {
		if (g_mgmt_sim->hw_msg_count == 0) {
			/* First message: updateTomaKeepaliveToken */
			payload = malloc(256);
			if (payload)
				len = (size_t)snprintf(payload, 256, mgmt_simu_kafka_msg_templates[1],
						       g_mgmt_sim->hostname);
			g_mgmt_sim->hw_msg_count++;
		} else if ((g_mgmt_sim->hw_msg_count++ % 4) == 0) {
			/* Periodically inject hardwareConfiguration */
			payload = malloc(4096);
			if (payload)
				len = (size_t)snprintf(payload, 4096, mgmt_simu_kafka_msg_templates[3],
						       g_mgmt_sim->hw_msg_count, g_mgmt_sim->hostname);
		}
	}
	/* CMD consumer - command messages */
	else if (strncmp(consumer_name, "CMD", 3) == 0) {
		if (g_mgmt_sim->cmd_msg_count == 0) {
			/* First command: updateTomaKeepaliveToken (zone) */
			payload = malloc(256);
			if (payload)
				len = (size_t)snprintf(payload, 256, mgmt_simu_kafka_msg_templates[1],
						       g_mgmt_sim->hostname);
			g_mgmt_sim->cmd_msg_count++;
		} else if ((g_mgmt_sim->cmd_msg_count++ % 3) == 0) {
			/* Periodically send updateLeaderKeepaliveToken */
			payload = strdup(mgmt_simu_kafka_msg_templates[0]);
			if (payload)
				len = strlen(payload);
		}
	}
	/* Target consumer (not incrementalTarget) - addTarget messages */
	else if (strstr(consumer_name, "incrementalTarget") == NULL) {
		if (g_mgmt_sim->target_msg_count == 0) {
			/* First message: addTarget (self as 1-machine raft domain) */
			payload = malloc(256);
			if (payload)
				len = (size_t)snprintf(payload, 256, mgmt_simu_kafka_msg_templates[2],
						       g_mgmt_sim->hostname);
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

	if (!g_mgmt_sim || !payload || len == 0)
		return;

	/* Check if this is a reportTarget message */
	m_type = strstr((const char *)payload, "\"messageType\":");
	if (!m_type)
		return;

	is_report_target = (strncmp(m_type, "\"messageType\": \"reportTarget\"", 28) == 0);
	if (is_report_target) {
		free(g_mgmt_sim->last_report_target_json);
		g_mgmt_sim->last_report_target_json = strndup((const char *)payload, len);
	}
}

const char *mgmt_sim_get_last_report_target(void)
{
	if (!g_mgmt_sim)
		return NULL;
	return g_mgmt_sim->last_report_target_json;
}

void mgmt_sim_destroy(void)
{
	if (!g_mgmt_sim)
		return;

	free(g_mgmt_sim->last_report_target_json);
	free(g_mgmt_sim);
	g_mgmt_sim = NULL;
}
