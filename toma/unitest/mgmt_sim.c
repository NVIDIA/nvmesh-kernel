/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "mgmt_sim.h"
#include "kafka/sandbox_kafka_public.h"
#include "kafka/sandbox_kafka_internal.h"
#include "nvmeibt_json_base.h"
#include "nvmeibt_debug.h"	// Binary tracing

#define MGMT_DB_UUID_JSON "\"dbUUID\":\"141d3140-c3c0-11f0-bc49-e391b6ca4c2b\""

/* Volume scenario constants */
#define UUID_from_U32(WHAT) WHAT "-0000-0000-0000-000000000000"
#define V_R1_VOL_UUID            UUID_from_U32("aaa00000")
#define V_R1_PRAID_UUID          UUID_from_U32("aaa001a0")
#define DISK_UUID_LOCAL_002      UUID_from_U32("d0020000")
#define DISK_UUID_LOCAL_003      UUID_from_U32("d0030000")
#define DISK_UUID_REMOTE38_D0    UUID_from_U32("f38cebd0")
#define DISK_UUID_REMOTE38_D1    UUID_from_U32("f38cebd1")
#define DISK_UUID_REMOTE39_D0    UUID_from_U32("f39cebd0")
#define DISK_UUID_REMOTE39_D1    UUID_from_U32("f39cebd1")

enum e_disk_format_state {
	FMT_IDLE = 'I',
	FMT_SENT = 'S',
	FMT_IN_PROGRESS = 'P',
	FMT_ZEROING = 'Z',
	FMT_DONE = 'D',
};

static int make_msg_update_leader_keepalive_token(char *buf, size_t capacity) {
	return snprintf(buf, capacity, "{\"messageType\":\"updateLeaderKeepaliveToken\""
		",\"messageTypeVersion\":1,\"payload\":{\"token\":1,\"keepaliveInterval\":1}}");
}

/* V_REMOTE1: RAID-1, segments only on remote disks (D0_n38, D0_n39) */
static int make_msg_add_volume_remote1(char *buf, size_t capacity)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"addVolume\",\"messageTypeVersion\":1"
		",\"payload\":{\"_id\":\"V_REMOTE1\",\"uuid\":\"bbb00100-0000-0000-0000-000000000001\""
		",\"version\":1,\"name\":\"V_REMOTE1\",\"blockSize\":4096"
		",\"lockServer\":{\"maxNOwners\":2,\"type\":4,\"locksetShift\":-1}"
		",\"blocks\":1024,\"RAIDLevel\":\"Mirrored RAID-1\""
		",\"numberOfMirrors\":1,\"stripeSize\":32,\"stripeWidth\":1,\"status\":\"unavailable\""
		",\"action\":\"initializing\",\"relativeRebuildPriority\":10"
		",\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null"
		",\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,"
		"\"chunks\":[{\"uuid\":\"bbb001c0-0000-0000-0000-000000000010\",\"vlbs\":0,\"vlbe\":1023,\"pRaids\":["
			"{\"uuid\":\"bbb001a0-0000-0000-0000-000000000011\",\"activated\":false,\"stripeIndex\":0,\"zone\":\"1\",\"diskSegments\":["
				"{\"uuid\":\"bbb001e1-0000-0000-0000-000000000012\",\"lbs\":0,\"lbe\":1023,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" DISK_UUID_REMOTE38_D0 "\"},"
				"{\"uuid\":\"bbb001e2-0000-0000-0000-000000000013\",\"lbs\":0,\"lbe\":1023,\"type\":\"data\",\"pRaidIndex\":1,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" DISK_UUID_REMOTE39_D0 "\"}"
		"]}]}]}}");
}

/* V_R1: RAID-1, one local segment on NVMD_SN_003.1 + one remote on D0_n38 */
static int make_msg_add_volume_r1(char *buf, size_t capacity)
{
	return snprintf(buf, capacity,
		"{\"messageType\":\"addVolume\",\"messageTypeVersion\":1"
		",\"payload\":{\"_id\":\"V_R1\",\"uuid\":\"" V_R1_VOL_UUID "\""
		",\"version\":1,\"name\":\"V_R1\",\"blockSize\":4096"
		",\"lockServer\":{\"maxNOwners\":3,\"type\":4,\"locksetShift\":-1}"
		",\"blocks\":1024,\"RAIDLevel\":\"Mirrored RAID-1\""
		",\"numberOfMirrors\":2,\"stripeSize\":32,\"stripeWidth\":1,\"status\":\"unavailable\""
		",\"action\":\"initializing\",\"relativeRebuildPriority\":10"
		",\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null"
		",\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,"
		"\"chunks\":["
		"{\"uuid\":\"aaa001c0-0000-0000-0000-000000000010\",\"vlbs\":0,\"vlbe\":1023,\"pRaids\":["
			"{\"uuid\":\"" V_R1_PRAID_UUID "\",\"activated\":false,\"stripeIndex\":0,\"zone\":\"1\",\"diskSegments\":["
				"{\"uuid\":\"aaa001e1-0000-0000-0000-000000000000\",\"lbs\":6176,\"lbe\":7199,\"type\":\"data\",\"pRaidIndex\":0,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" DISK_UUID_LOCAL_003 "\"},"
				"{\"uuid\":\"aaa001e2-0000-0000-0000-000000000000\",\"lbs\":1024,\"lbe\":2047,\"type\":\"data\",\"pRaidIndex\":1,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" DISK_UUID_REMOTE38_D0 "\"},"
				"{\"uuid\":\"aaa001e3-0000-0000-0000-000000000000\",\"lbs\":0" ",\"lbe\":1023,\"type\":\"data\",\"pRaidIndex\":2,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" DISK_UUID_REMOTE38_D1 "\"}"
		"]}]}]}}");
}

static int make_msg_delete_volume_r1(char *buf, size_t capacity) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"deleteVolume\",\"messageTypeVersion\":1,\"payload\":{"
		"\"_id\":\"V_R1\",\"uuid\":\"" V_R1_VOL_UUID "\",\"name\":\"V_R1\",\"version\":1}}");
}

static int make_msg_delete_volume_completed_r1(char *buf, size_t capacity) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"deleteVolumeCompleted\",\"messageTypeVersion\":1,\"payload\":{"
		"\"_id\":\"V_R1\",\"uuid\":\"" V_R1_VOL_UUID "\",\"name\":\"V_R1\"}}");
}

/* Forward declarations */
static void mgmt_sim_parse_report_target(struct mm_json_elem *root);
static void mgmt_sim_parse_praid_report(struct mm_json_elem *root);

/* Per-disk status extracted from reportTarget */
struct mgmt_sim_disk_status {			// Todo: maybe move to cfg?
	const char *disk_id;				// Disk name
	const char *uuid;
	char status[16];					// As reported by Toma
	struct t_format_monitor {
		unsigned counter_sent;			// Ever increasing generation for for disk format cmd to toma. Value sent in last formatDrive
		unsigned counter_toma_reply_done;
		unsigned counter_toma_reply_in_progress;
		int      msg_seq;				// Msg's can arrive unordered and multiple times. Use sequence to discared already processed messages
		enum e_disk_format_state state;	// per-drive format tracking
	} format;
	u16 vendor;
	u16 block_size;						// In bytes
	u16 metadata_size;					// In bytes
};

static int make_msg_format_drive(char *buf, size_t capacity, const struct mgmt_sim_disk_status *d, unsigned long boot_time) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"formatDrive\",\"messageTypeVersion\":1"
		",\"payload\":{\"diskID\":\"%s\",\"uuid\":\"%s\",\"vendor\":%u"
		",\"formatType\":\"format_ec\",\"formatRequestCounter\":%u"
		",\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu"
		", " MGMT_DB_UUID_JSON "}}",
		d->disk_id, d->uuid, d->vendor, d->format.counter_sent, boot_time);
}

/* Management simulator state */
struct mgmt_sim_state {
	struct sb_cluster_conf *cfg;

	struct t_hw_config_queue_state {	// Per-consumer state for deterministic message sequencing
		int msg_count, conf_version;
	} hw;
	struct t_cmd_queue_state {
		int msg_count;
	} cmd;
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

	/* Per-Producer state for deterministic message sequencing */
	int n_leader_keep_alives;
	uint32_t raftTerm;					// AS reported by Toma leader

	/* Test scenario state */
	int64_t boot_time;                      /* from reportTarget payload.node.bootTime */
	struct mgmt_sim_disk_status disk_002;   /* NVMD_SN_002.1 */
	struct mgmt_sim_disk_status disk_003;   /* NVMD_SN_003.1 */
	bool v_r1_praid_reported;               /* updatePRaidReport contained V_R1's pRaid UUID */
	bool got_report_target;                 /* reportTarget received since last FSM transition */
	bool v_r1_seg_zeroing_progress_seen;    /* segmentZeroingProgress received for V_R1 praid */
	bool v_r1_praid_deprecated;             /* updatePRaidReport shows all V_R1 segs "deprecated" */
	bool v_r1_delete_completed_sent;        /* deleteVolumeCompleted was sent */
	bool v_r1_praid_absent_from_report;     /* praid report received without V_R1 (garbage collected) */
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

	m->disk_002.disk_id = "NVMD_SN_002.1";
	m->disk_003.disk_id = "NVMD_SN_003.1";
	m->disk_002.uuid = DISK_UUID_LOCAL_002;
	m->disk_003.uuid = DISK_UUID_LOCAL_003;
	m->disk_002.vendor = 5122;
	m->disk_003.vendor = 5123;
	m->disk_002.format.counter_sent = 20;		// Start from some number, different start for each disk for easier logs analysis
	m->disk_003.format.counter_sent = 30;
	m->disk_003.format.state = m->disk_002.format.state = FMT_IDLE;
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

static bool __is_disk_fmt_running(enum e_disk_format_state e) { return ((e != FMT_IDLE) && (e != FMT_DONE)); }

static struct mgmt_sim_disk_status *__lookup_disk_by_name(const char *drive_name) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	if (strcmp(drive_name, m->disk_002.disk_id) == 0) return &m->disk_002;
	if (strcmp(drive_name, m->disk_003.disk_id) == 0) return &m->disk_003;
	BUG_ON(drive_name[0] != 'S'); 		// For now, ignore stock drivers in Toma report
	return NULL;
}

static struct mgmt_sim_disk_status *__lookup_disk_by_uuid(const char *disk_uuid) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	if (strcmp(disk_uuid, m->disk_002.uuid) == 0) return &m->disk_002;
	if (strcmp(disk_uuid, m->disk_003.uuid) == 0) return &m->disk_003;
	BUG_ON(true); return NULL;
}

static void __send_format_drive_msg(const struct mgmt_sim_disk_status *d) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(1024);
	size_t len = (size_t)make_msg_format_drive(buf, 1024, d, (unsigned long)m->boot_time);
	N_IMf(__AUTOID__, "sending formatDrive disk=@STR format_gen=@INT, bootTime=@INT64_TD", d->disk_id, d->format.counter_sent, m->boot_time);
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
}

static void __check_format_progress(struct mgmt_sim_disk_status *d, int msg_seq, bool on_report_target_msg) {
	if (msg_seq <= d->format.msg_seq) {
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], old message, seq=@INT <= @INT, ignoring", d->disk_id, d->format.state, d->format.state, msg_seq, d->format.msg_seq);
	} else if (__is_disk_fmt_running(d->format.state)) {
		const unsigned expected = d->format.counter_sent;
		enum e_disk_format_state prev_state = d->format.state;
		if ((d->format.counter_toma_reply_done == expected) && (strcmp(d->status, "Ok") == 0)) {
			d->format.state = FMT_DONE;
		} else if (d->format.counter_toma_reply_in_progress == expected) {
			if (!on_report_target_msg)				// Zeroing message
				d->format.state = FMT_ZEROING;
			else if (prev_state == FMT_SENT)
				d->format.state = FMT_IN_PROGRESS;
		} else {
			BUG_ON(prev_state != FMT_SENT);			// Incorrect transition
			__send_format_drive_msg(d);
		}
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], seq=@INT, format_gen=@INT, @STR[report]", d->disk_id, prev_state, d->format.state, msg_seq, expected, on_report_target_msg ? "Target" : "Zeroin");
		d->format.msg_seq = msg_seq;
	} else {
		BUG_ON(!on_report_target_msg);				// Illegal to receive zeroing message when no format is running
	}
}

void mgmt_sim_send_msg_assign_to_zone(int zone_idx) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	const size_t capacity = 256;
	char *msg = malloc(capacity);
	const size_t len = snprintf(msg, capacity,
		"{\"messageType\":\"updateTomaKeepaliveToken\",\"messageTypeVersion\":1"
		",\"payload\":{\"nodeID\":\"%s\",\"token\":3,\"zone\":\"%d\",\"keepaliveInterval\":1}}",
		m->cfg->live->hostname, zone_idx);
	m->cfg->zone_idx = zone_idx;
	sim_broker_topic_msg_produce(g_mgmt_sim->k_producers.cmd, msg, len, false);
	m->cmd.msg_count++;
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
	struct mgmt_sim_state *m = g_mgmt_sim;
	const struct sb_node_conf *other_toma = m->cfg->other;
	const size_t capacity = 4096;
	char *msg = malloc(capacity);
	const int config_ver = (++m->hw.conf_version);				// As if something in configuration changed
	const int kafka_seq =  (++m->hw.msg_count);
	const size_t len = snprintf(msg, capacity,
		"{\"messageType\":\"hardwareConfiguration\",\"messageTypeVersion\":1,\"payload\":{\"managementConfiguration\":{\"_id\":\"1\""
		",\"configurationVersion\":%d,\"leaderToken\":1,\"kafkaMessageSequence\":%d,\"raftTerm\":9"
		",\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},"
		"\"targets\":["
			"{\"_id\":\"nvme37.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"%s\",\"blocks\":32768,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":%d,\"uuid\":\"%s\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"%s\",\"blocks\":32768,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":%d,\"uuid\":\"%s\",\"version\":7,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5d008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5d009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c0-11f0-bc49-e391b6ca4c2b\"}]},"
			"{\"_id\":\"nvme38.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"D0_n38\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5122,\"uuid\":\"" DISK_UUID_REMOTE38_D0 "\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"D1_n38\",\"blocks\":2000,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5123,\"uuid\":\"" DISK_UUID_REMOTE38_D1 "\",\"version\":7,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5e008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef0-c3c1-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5e009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0a0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce10-c3c1-11f0-bc49-e391b6ca4c2b\"}]},"
			"{\"_id\":\"nvme39.mlnx\",\"node_id\":\"%s\",\"uuid\":\"%s\","
				"\"disks\":["
				"{\"diskID\":\"D0_n39\",\"blocks\":195353046,\"block_size\":4096,\"activeFormatRequestCounter\":1,\"vendorID\":5197,\"uuid\":\"" DISK_UUID_REMOTE39_D0 "\",\"version\":7,\"isOutOfService\":false},"
				"{\"diskID\":\"D1_n39\",\"blocks\":195353046,\"block_size\":1024,\"activeFormatRequestCounter\":0,\"vendorID\":3333,\"uuid\":\"" DISK_UUID_REMOTE39_D1 "\",\"version\":1,\"isOutOfService\":false}],"
				"\"nics\":["
					"{\"nicID\":\"0x0000000000000000bae924fffee5f008\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0b0126\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4cef2-c3c0-11f0-bc49-e391b6ca4c2b\"},"
					"{\"nicID\":\"0x0000000000000000bae924fffee5f009\",\"protocol\":\"RoCE\""
						",\"guid\":\"0x00000000000000000000ffff0a0b0226\",\"pkey\":65535,\"version\":1,\"uuid\":\"cff4ce12-c3c0-11f0-bc49-e391b6ca4c2b\"}]}"
		"]}}",
		config_ver, kafka_seq,
		m->cfg->live->hostname, m->cfg->live->uuid,
			m->disk_002.disk_id, m->disk_002.vendor, m->disk_002.uuid,
			m->disk_003.disk_id, m->disk_003.vendor, m->disk_003.uuid,
		other_toma[0].hostname, other_toma[0].uuid,
		other_toma[1].hostname, other_toma[1].uuid);
	sim_broker_topic_msg_produce(g_mgmt_sim->k_producers.hw, msg, len, false);
}

static void __handle_low_prio_msg(const rd_kafka_message_t *msg) {
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	const uint64_t msg_seq = json_get_dict_num(root, "messageSequence", ~0UL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
	if (strcmp(message_type, "driveZeroingProgress") == 0) {
		struct mm_json_elem *payload = json_get_dict_value(root, "payload");
		const char *disk_uuid = json_get_dict_str(payload, "diskUUID", NULL);
		struct mgmt_sim_disk_status *d = __lookup_disk_by_uuid(disk_uuid);
		__check_format_progress(d, (int)msg_seq, false);
	} else if (strcmp(message_type, "updateDiskSegmentsDirtyBits") == 0) {
		/* silently ignore */
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

static void __handle_keepalive_msg(const rd_kafka_message_t *msg) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	BUG_ON(!m);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
	if (strcmp(message_type, "leaderKeepalive") == 0) {
		struct mm_json_elem *payload = json_get_dict_value(root, "payload");
		m->raftTerm = json_get_dict_num(payload, "raftTerm", 0);
		m->n_leader_keep_alives++;
		N_Tf(__AUTOID__, "<< Leader KAL {raftTerm=@INT, gen=@INT}", m->raftTerm, m->n_leader_keep_alives);
	} else if (strcmp(message_type, "keepalive") == 0) {
		// {"originType":"TOMA","messageType":"keepalive","messageTypeVersion":2,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":11831,"leaderToken":null,"keepaliveInterval":5,"payload":{"zone":"1","leaderUUID":"nvme39.nvidia.com","bootTime":1767279770145,"featureCompatibilityVersion":"0","tomaSoftwareVersion":"784","version":"3.3.0-1332","buildNumber":"","rebuildStats":{"nRunningDirtyRebuild":0,"nPendingDirtyRebuild":0,"nRunningStaleRebuild":0,"nPendingStaleRebuild":6,"nRunningTxidRebuild":0,"nPendingTxidRebuild":0,"nRunningColdRecovery":0,"nPendingColdRecovery":0,"nRunningJGCRebuild":0,"nPendingJGCRebuild":0,"nRunningScrubbing":0,"nPendingScrubbing":3}}}
	} else {
		BUG_ON(true);
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

static void __handle_priority_msg(const rd_kafka_message_t *msg) {
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
		if (strcmp(message_type, "reportTarget") == 0) {
			// {"originType":"TOMA","messageType":"reportTarget","messageTypeVersion":1,"hostname":"nvme37.nvidia.com","tomaToken":2,"messageSequence":24,"leaderToken":null,"payload":{"node":{"zone":"1","bootTime":1767535819492,"cpu_temp":"30.0","version":"3.3.0-1340","buildNumber":"","reportID":25,"branch":"master","commit":"18af9e759c828cc1d3776bfdd6cb68ade54ef738","configProfile":{"id":"569407c0-e976-11f0-b6cb-871ca30885b4","name":"Cluster Default","version":"1"},"node_status":"1","node_id":"nvme37.nvidia.com","targetUpdatesSequence":4,"cpu_load":"0.0","disks":[{"diskID":"S3HCNX0JC01918.1","disk_version":0,"blocks":1562500000,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01918","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"1_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5f","Power_On_Hours":"0x10049","Unsafe_Shutdowns":"0x56","Media_Errors":"0x2","Number_of_Error_Information_Log_Entries":"0x1a5d","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":25439882564,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700220.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700220","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xb4","Power_On_Hours":"0xfb7f","Unsafe_Shutdowns":"0x94","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x5980","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4546392587,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0JC01904.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0JC01904","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"2_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x5e","Power_On_Hours":"0x10046","Unsafe_Shutdowns":"0x54","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x2beb","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":49694267592,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3P8NY0J700164.1","disk_version":0,"blocks":937703088,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3P8NY0J700164","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZQKW480HMHQ-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x6e","Power_On_Hours":"0x1062c","Unsafe_Shutdowns":"0x5c","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x46","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":4675551222,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S3HCNX0K600397.1","disk_version":0,"blocks":1562824368,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S3HCNX0K600397","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL800HEHP-00003","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"0_%","Controller_Busy_Time":"0x0","Power_Cycles":"0xca","Power_On_Hours":"0xe767","Unsafe_Shutdowns":"0xb5","Media_Errors":"0x3","Number_of_Error_Information_Log_Entries":"0x165c","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":29489346822,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"disID":"S4C9NF0M500226.1","disk_version":0,"blocks":3125627568,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S4C9NF0M500226","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZWLL1T6HAJQ-00005","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"4_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x56","Power_On_Hours":"0xda5d","Unsafe_Shutdowns":"0x49","Media_Errors":"0x57","Number_of_Error_Information_Log_Entries":"0x1c5a","status":"Not_Initialized","isExcluded":false,"excludeReason":"None","metadataCapabilities":"3","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":512,"metaBS":8},{"dataBS":4096,"metaBS":0},{"dataBS":4096,"metaBS":8}],"writeCounter":303100870572,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0},{"diskID":"S665NE0R702075.1","disk_version":0,"blocks":1875385008,"block_size":512,"metadata_size":0,"pci_address":"","Serial_Number":"S665NE0R702075","nsid":1,"Vendor":"0x144d","Model":"SAMSUNG MZ1L2960HCJR-00A07","Submission_Queues":0,"Completion_Queues":0,"MSIX_Interrupts":0,"Numa_Node":0,"Critical_Warning":"0x0","Available_Spare":"100_%","Available_Spare_Threshold":"10_%","Percentage_Used":"3_%","Controller_Busy_Time":"0x0","Power_Cycles":"0x70","Power_On_Hours":"0x817d","Unsafe_Shutdowns":"0x53","Media_Errors":"0x0","Number_of_Error_Information_Log_Entries":"0x0","status":"Not_Initialized","isExcluded":true,"excludeReason":"In-Use","metadataCapabilities":"0","formatOptions":[{"dataBS":512,"metaBS":0},{"dataBS":4096,"metaBS":0}],"writeCounter":10392940702,"reappearingCounter":2,"formatRequestCounter":0,"activeFormatRequestCounter":0}],"nics":[{"nicID":"0x0000000000000000bae924fffee5cfd8","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0125","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_2"},{"nicID":"0x0000000000000000bae924fffee5cfd9","protocol":1,"status":1,"guid":"0x00000000000000000000ffff0a0a0225","pkey":"0xffff","pci_root":0,"mtu":4096,"deviceType":"mlx5_3"}]}}}
			mgmt_sim_parse_report_target(root);
		} else  if (strcmp(message_type, "updatePRaidReport") == 0) {
			mgmt_sim_parse_praid_report(root);
		 	// {"originType":"TOMA","messageType":"updatePRaidReport","messageTypeVersion":1,"hostname":"nvme39.nvidia.com","tomaToken":2,"messageSequence":85,"leaderToken":1,"payload":{"pRaidsUpdate":[{"uuid":"b60b04b1-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60b04b0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60b2bc0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]},{"uuid":"b60ab692-e97b-11f0-995c-3792ee0db955","raftTerm":5,"pRaidMinorVersion":0,"pRaidMajorVersion":257,"isRaftLeader":1,"segments":[{"segmentID":"b60ab691-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"},{"segmentID":"b60adda0-e97b-11f0-995c-3792ee0db955","status":"booting","vitality":"up"}]}]}}
		} else if (strcmp(message_type, "segmentZeroingProgress") == 0) {
			// {"originType":"TOMA","messageType":"segmentZeroingProgress","messageTypeVersion":1,"hostname":"nvme34.nvidia.com","tomaToken":2,"messageSequence":335,"leaderToken":null,"payload":{"praidVersion":"258.0","segmentUUID":"98e46d20-ea22-11f0-bad8-af65dd8e6ead","pRaidUUID":"98e44612-ea22-11f0-bad8-af65dd8e6ead","nZeroedBlks":262144}}
			struct mm_json_elem *payload = json_get_dict_value(root, "payload");
			const char *praid_uuid = json_get_dict_str(payload, "pRaidUUID", NULL);
			if (praid_uuid && strcmp(praid_uuid, V_R1_PRAID_UUID) == 0) {
				g_mgmt_sim->v_r1_seg_zeroing_progress_seen = true;
				N_IMf(msim_szp, "segmentZeroingProgress: V_R1 praid matched");
			}
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

void mgmt_sim_verify_at_end(void) {
	BUG_ON(!g_mgmt_sim);
	BUG_ON((g_mgmt_sim->n_leader_keep_alives <= 0));
}

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
		else BUG_ON(true);	// Unsupported topic
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
}

/******************************************************************************/
/* Static helper functions                                                    */
/******************************************************************************/
static void __extract_disks_status_from_report_target_msg(struct mm_json_elem *disks_array, uint64_t msg_seq) {
	int i;
	for (i = 0; i < disks_array->array.len; i++) {
		struct mm_json_elem *disk_elem = disks_array->array.elements[i];
		const char *disk_id = json_get_dict_str(disk_elem, "diskID", NULL);
		struct mgmt_sim_disk_status *d = __lookup_disk_by_name(disk_id);
		BUG_ON(!disk_elem || (disk_elem->type != JSON_E_DICT) || !disk_id);
		if (!d) continue;		// Ignore stock drivers

		nvmeibt_strlcpy(d->status, json_get_dict_str(disk_elem, "status", "unknown"), sizeof(d->status));
		d->format.counter_toma_reply_done =        (unsigned)json_get_dict_num(disk_elem, "formatRequestCounter", -1);
		d->format.counter_toma_reply_in_progress = (unsigned)json_get_dict_num(disk_elem, "activeFormatRequestCounter", -1);
		d->block_size = json_get_dict_num(disk_elem, "block_size", -1);
		d->metadata_size = json_get_dict_num(disk_elem, "metadata_size", -1);
		N_Tf(msim_disk, "disk=@STR status=@STR frc=@INT afrc=@INT, @UINT+@UINT[b]", d->disk_id, d->status, d->format.counter_toma_reply_done, d->format.counter_toma_reply_in_progress, d->block_size, d->metadata_size);
		__check_format_progress(d, (int)msg_seq, true);
	}
}

static void mgmt_sim_parse_report_target(struct mm_json_elem *root) {
	struct mm_json_elem *payload = json_get_dict_value(root,    "payload");
	struct mm_json_elem *node =    json_get_dict_value(payload, "node");
	struct mm_json_elem *disks =   json_get_dict_value(node,    "disks");
	const uint64_t msg_seq = json_get_dict_num(root, "messageSequence", ~0UL);
	struct mgmt_sim_state *m = g_mgmt_sim;

	m->boot_time = json_get_dict_num(node, "bootTime", 0);
	if (disks && (disks->type == JSON_E_ARRAY))
		__extract_disks_status_from_report_target_msg(disks, msg_seq);
	m->got_report_target = true;
	// N_Tf(__AUTOID__, "reportTarget bootTime=@INT64_TD", m->boot_time);
}

static void mgmt_sim_parse_praid_report(struct mm_json_elem *root) {
	struct mm_json_elem *payload = json_get_dict_value(root, "payload");
	struct mm_json_elem *praids_update = json_get_dict_value(payload, "pRaidsUpdate");
	bool v_r1_found = false;
	if (!praids_update || praids_update->type != JSON_E_ARRAY)
		return;
	for (int i = 0; i < praids_update->array.len; i++) {
		struct mm_json_elem *entry = praids_update->array.elements[i];
		const char *uuid;
		if (!entry || entry->type != JSON_E_DICT)
			continue;
		uuid = json_get_dict_str(entry, "uuid", NULL);
		if (uuid && strcmp(uuid, V_R1_PRAID_UUID) == 0) {
			struct mm_json_elem *segments;
			N_IMf(msim_praid, "matched V_R1 pRaid UUID");
			g_mgmt_sim->v_r1_praid_reported = true;
			v_r1_found = true;

			/* Check if all segments have status "deprecated" */
			segments = json_get_dict_value(entry, "segments");
			if (segments && segments->type == JSON_E_ARRAY && segments->array.len > 0) {
				bool all_deprecated = true;
				int j;
				for (j = 0; j < segments->array.len; j++) {
					struct mm_json_elem *seg = segments->array.elements[j];
					const char *status = json_get_dict_str(seg, "status", "");
					if (strcmp(status, "deprecated") != 0) {
						all_deprecated = false;
						break;
					}
				}
				if (all_deprecated) {
					g_mgmt_sim->v_r1_praid_deprecated = true;
					N_IMf(msim_praid_dep, "V_R1 all segments deprecated");
				}
			}
		}
	}
	/* After deleteVolumeCompleted: if V_R1 praid is absent, it was garbage collected */
	if (g_mgmt_sim->v_r1_delete_completed_sent && !v_r1_found) {
		g_mgmt_sim->v_r1_praid_absent_from_report = true;
		N_IMf(msim_praid_gc, "V_R1 praid absent (garbage collected)");
	}
}

/******************************************************************************/
/* Condition-query functions for fiber-based test scenario                     */
/******************************************************************************/
static bool __disk_ready_for_format(const struct mgmt_sim_disk_status *d) {
	return (strcmp(d->status, "Ok") == 0) || (strcmp(d->status, "Not_Initialized") == 0);
}

bool mgmt_sim_both_disks_ready_for_format(void) {
	const struct mgmt_sim_state *m = g_mgmt_sim;
	return __disk_ready_for_format(&m->disk_002) && __disk_ready_for_format(&m->disk_003);
}

bool mgmt_sim_consume_got_report_target(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	if (m->got_report_target) {
		m->got_report_target = false;
		return true;
	}
	return false;
}

int mgmt_sim_get_n_leader_keep_alives_received(void) {
	return g_mgmt_sim->n_leader_keep_alives;
}

bool mgmt_sim_v_r1_praid_reported(void) {
	return g_mgmt_sim->v_r1_praid_reported;
}

bool mgmt_sim_v_r1_seg_zeroing_seen(void)        { return g_mgmt_sim->v_r1_seg_zeroing_progress_seen; }
bool mgmt_sim_v_r1_praid_deprecated(void)         { return g_mgmt_sim->v_r1_praid_deprecated; }
bool mgmt_sim_v_r1_praid_absent_from_report(void) { return g_mgmt_sim->v_r1_praid_absent_from_report; }


/******************************************************************************/
/* Message-sender functions for fiber-based test scenario                      */
/******************************************************************************/
void mgmt_sim_send_format_drive(const char *drive_name) {
	struct mgmt_sim_disk_status *d = __lookup_disk_by_name(drive_name);
	BUG_ON(__is_disk_fmt_running(d->format.state));
	d->format.counter_sent++;
	d->format.state = FMT_SENT;
	__send_format_drive_msg(d);
}

bool mgmt_sim_drive_format_is_done(const char *drive_name) {
	struct mgmt_sim_disk_status *d = __lookup_disk_by_name(drive_name);
	BUG_ON(d->format.state == FMT_IDLE); // should only be called after sending a format command
	return d->format.state == FMT_DONE;
}

void mgmt_sim_send_leader_keep_alive(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *payload = malloc(256);
	const size_t len = make_msg_update_leader_keepalive_token(payload, 256);
	sim_broker_topic_msg_produce(m->k_producers.l_vol, payload, len, false);
}

void mgmt_sim_send_add_volume_remote1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(2048);
	int len = make_msg_add_volume_remote1(buf, 2048);
	N_IMf(msim_fsm4, "sending addVolume V_REMOTE1");
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, len, false);
}

void mgmt_sim_send_add_volume_r1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(2048);
	int len = make_msg_add_volume_r1(buf, 2048);
	N_IMf(msim_fsm5, "sending addVolume V_R1");
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, len, false);
}

void mgmt_sim_send_delete_volume_r1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(512);
	int len = make_msg_delete_volume_r1(buf, 512);
	N_IMf(msim_del1, "sending deleteVolume V_R1");
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, len, false);
}

void mgmt_sim_send_delete_volume_completed_r1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(512);
	int len = make_msg_delete_volume_completed_r1(buf, 512);
	N_IMf(msim_del3, "sending deleteVolumeCompleted V_R1");
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, len, false);
	m->v_r1_delete_completed_sent = true;
}
