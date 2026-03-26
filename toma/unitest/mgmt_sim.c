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
#define UUID_from_U32 			 "%8x-0000-0000-0000-000000000000"

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

/* Forward declarations */
static void mgmt_sim_parse_report_target(struct mm_json_elem *root);
static void mgmt_sim_parse_praid_report(struct mm_json_elem *root);

struct mgmt_sim_disk_status {			// Per-disk status extracted from reportTarget
	const struct sb_disk_conf *conf;	// Configuration static non changing info
	char status[16];					// As reported by Toma
	struct t_format_monitor {
		unsigned counter_sent;			// Ever increasing generation for for disk format cmd to toma. Value sent in last formatDrive
		unsigned counter_toma_reply_done;
		unsigned counter_toma_reply_in_progress;
		int      msg_seq;				// Msg's can arrive unordered and multiple times. Use sequence to discared already processed messages
		enum e_disk_format_state state;	// per-drive format tracking
	} format;
	u16 block_size;						// In bytes
	u16 metadata_size;					// In bytes
};

static int make_msg_format_drive(char *buf, size_t capacity, const struct mgmt_sim_disk_status *d, unsigned long boot_time) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"formatDrive\",\"messageTypeVersion\":1"
		",\"payload\":{\"diskID\":\"%s\",\"uuid\":\"" UUID_from_U32 "\",\"vendor\":%u"
		",\"formatType\":\"format_ec\",\"formatRequestCounter\":%u"
		",\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu"
		", " MGMT_DB_UUID_JSON "}}",
		d->conf->name, d->conf->uuid, d->conf->vendor, d->format.counter_sent, boot_time);
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
	struct mgmt_sim_disk_status disks_st[3];	// Toma report of up to N disks
	bool v_r1_praid_reported;               /* updatePRaidReport contained V_R1's pRaid UUID */
	bool got_report_target;                 /* reportTarget received since last FSM transition */
	bool v_r1_seg_zeroing_progress_seen;    /* segmentZeroingProgress received for V_R1 praid */
	bool v_r1_praid_deprecated;             /* updatePRaidReport shows all V_R1 segs "deprecated" */
	bool v_r1_delete_completed_sent;        /* deleteVolumeCompleted was sent */
	bool v_r1_praid_absent_from_report;     /* praid report received without V_R1 (garbage collected) */
};

static struct mgmt_sim_state *g_mgmt_sim = NULL;

struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg) {
	struct mgmt_sim_state *m = g_mgmt_sim = calloc(1, sizeof(*g_mgmt_sim));
	int d;
	m->cfg = initialized_cfg;
	for (d = 0; d < (int)ARRAY_SIZE(initialized_cfg->live->disks); d++) {
		m->disks_st[d].conf = &initialized_cfg->live->disks[d];
		m->disks_st[d].format.counter_sent = 20 + (10 * d);		// Start from different number for each disk for easier logs analysis
		m->disks_st[d].format.state = FMT_IDLE;
	}
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

static void __send_msg_volume_add(int vol_idx) {
	#define BUF_ADD(...) rv += snprintf(&buf[rv], msg_size-rv, __VA_ARGS__)
	struct mgmt_sim_state *m = g_mgmt_sim;
	const struct sb_volume_conf *V = &m->cfg->vols[vol_idx];
	const size_t msg_size = 2048;
	char *buf = malloc(msg_size);
	unsigned c, r, s;
	int rv = 0;
	BUG_ON(vol_idx >= m->cfg->n_vols);
	BUF_ADD("{\"messageType\":\"addVolume\",\"messageTypeVersion\":1,\"payload\":{\"_id\":\"%s\",\"uuid\":\"" UUID_from_U32 "\",\"version\":1,\"name\":\"%s\",\"blockSize\":4096,",
		V->name, V->uuid, V->name);
	BUF_ADD("\"lockServer\":{\"maxNOwners\":%u,\"type\":4,\"locksetShift\":-1},\"blocks\":%u,\"RAIDLevel\":\"Mirrored RAID-1\",\"numberOfMirrors\":%d,\"stripeSize\":32,\"stripeWidth\":%u,",
		V->chunks->raids->P+1, V->num_blocks, V->chunks->raids->P,  V->chunks->n_raids);
	BUF_ADD("\"status\":\"unavailable\",\"action\":\"initializing\",\"relativeRebuildPriority\":10,\"reservation\":{\"mode\":0,\"version\":1,\"reservedBy\":null,\"attachedClients\":[],\"lastTransitionDate\":null},\"use_debug_di\":false,");
	BUF_ADD("\"chunks\":[");
	for (c = 0; c < V->num_chunks; c++) {
		const struct sb_chunk_conf *pc = &V->chunks[c];
		BUF_ADD("{\"uuid\":\"" UUID_from_U32 "\",\"vlbs\":%u,\"vlbe\":%u,\"pRaids\":[", pc->uuid, pc->vlba_start, pc->vlba_end);
		for (r = 0; r < pc->n_raids; r++) {
			const struct sb_praid_conf *pr = &pc->raids[r];
			BUF_ADD("{\"uuid\":\"" UUID_from_U32 "\",\"activated\":false,\"stripeIndex\":%u,\"zone\":\"%d\",\"diskSegments\":[", pr->uuid, r, m->cfg->zone_idx);
			for (s = 0; s < (pr->D + pr->P); s++) {
				const struct sb_seg_conf *ps = &pr->segs[s];
				BUF_ADD("{\"uuid\":\"" UUID_from_U32 "\",\"lbs\":%u,\"lbe\":%u,\"type\":\"data\",\"pRaidIndex\":%u,\"pRaidTypeIndex\":0,\"status\":\"initializing\",\"diskUUID\":\"" UUID_from_U32 "\"},",
					ps->uuid, ps->block_start, ps->block_end, s, ps->disk_uuid);
			}
			rv--;	// Remove the last uneeded ','
		}
	}
	BUF_ADD("]}]}]}}");
	N_IMf(__AUTOID__, "vol=@STR sending msg addVolume, @INT[b]", m->cfg->vols[vol_idx].name, rv);
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, rv, false);
}

static void __send_msg_volume_del(int vol_idx, bool is_completed) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	const struct sb_volume_conf *V = &m->cfg->vols[vol_idx];
	const char *msg_type = (is_completed ? "deleteVolumeCompleted" : "deleteVolume" );
	const size_t msg_size = 512;
	char *buf = malloc(msg_size);
	int rv = 0;
	BUG_ON(vol_idx >= m->cfg->n_vols);
	BUF_ADD("{\"messageType\":\"%s\",\"messageTypeVersion\":1,\"payload\":{\"_id\":\"%s\",\"uuid\":\"" UUID_from_U32 "\",\"version\":1,\"name\":\"%s\"}}",
		msg_type, V->name, V->uuid, V->name);
	N_IMf(__AUTOID__, "vol=@STR sending msg @STR, @INT[b]", m->cfg->vols[vol_idx].name, msg_type, rv);
	sim_broker_topic_msg_produce(m->k_producers.l_vol, buf, rv, false);
}

static bool __is_disk_fmt_running(enum e_disk_format_state e) { return ((e != FMT_IDLE) && (e != FMT_DONE)); }

static struct mgmt_sim_disk_status *__lookup_disk_by_name(const char *drive_name) {
	return (drive_name[0] == 'S') ? NULL // For now, ignore stock drivers in Toma report
		: &g_mgmt_sim->disks_st[sb_cluster_get_disk_idx_from_disk_name(g_mgmt_sim->cfg, drive_name)];
}

static struct mgmt_sim_disk_status *__lookup_disk_by_uuid(const char *disk_uuid) {
	return &g_mgmt_sim->disks_st[sb_cluster_get_disk_idx_from_disk_uuid(g_mgmt_sim->cfg, disk_uuid)];
}

static void __send_format_drive_msg(const struct mgmt_sim_disk_status *d) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(1024);
	size_t len = (size_t)make_msg_format_drive(buf, 1024, d, (unsigned long)m->boot_time);
	N_IMf(__AUTOID__, "sending formatDrive disk=@STR format_gen=@INT, bootTime=@INT64_TD", d->conf->name, d->format.counter_sent, m->boot_time);
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
}

static void __check_format_progress(struct mgmt_sim_disk_status *d, int msg_seq, bool on_report_target_msg) {
	if (msg_seq <= d->format.msg_seq) {
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], old message, seq=@INT <= @INT, ignoring", d->conf->name, d->format.state, d->format.state, msg_seq, d->format.msg_seq);
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
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], seq=@INT, format_gen=@INT, @STR[report]", d->conf->name, prev_state, d->format.state, msg_seq, expected, on_report_target_msg ? "Target" : "Zeroin");
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
			"{\"nodeID\":\"%s\",\"uuid\":\"" UUID_from_U32 "\",\"targetsInZone\":%d,\"targetUpdatesSequence\":%d}}",
			msg_type, node->hostname, node->uuid, m->raft_quorum.num_nodes, m->raft_quorum.generation);
	m->raft_quorum.num_nodes += (do_add ? +1 : -1);
	N_Tf(__AUTOID__, "<< msg=@STR node=@STR, gen=@INT", msg_type, node->hostname, m->raft_quorum.generation);
	sim_broker_topic_msg_produce(m->k_producers.l_raft, buf, len, false);
}

void mgmt_sim_send_msg_latest_hw_config(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	size_t msg_size = 4096, rv = 0;
	char *buf = malloc(msg_size);
	const int config_ver = (++m->hw.conf_version);				// As if something in configuration changed
	const int kafka_seq =  (++m->hw.msg_count);
	int n, i;
	BUF_ADD("{\"messageType\":\"hardwareConfiguration\",\"messageTypeVersion\":1,\"payload\":{\"managementConfiguration\":{\"_id\":\"1\""
		",\"configurationVersion\":%d,\"leaderToken\":1,\"kafkaMessageSequence\":%d,\"raftTerm\":9"
		",\"stopSendingKeepaliveToken\":false," MGMT_DB_UUID_JSON "},\"targets\":[",
		config_ver, kafka_seq);
	for (n = 0; n < m->cfg->n_nodes; n++) {
		const struct sb_node_conf *N = &m->cfg->nodes[n];
		BUF_ADD("{\"_id\":\"%u\",\"node_id\":\"%s\",\"uuid\":\"" UUID_from_U32 "\",""\"disks\":[",
			N->uuid>>16, N->hostname, N->uuid);
		for (i = 0; i < (int)ARRAY_SIZE(N->disks); i++) {
			const struct sb_disk_conf *D = &N->disks[i];
			BUF_ADD("{\"diskID\":\"%s\",\"blocks\":%u,\"block_size\":%u,\"activeFormatRequestCounter\":1,\"vendorID\":%d,\"uuid\":\"" UUID_from_U32 "\",\"version\":7,\"isOutOfService\":%s},",
				D->name, D->size_bytes >> 12, 1 << 12, D->vendor, D->uuid, (D->is_out_of_service ? "true" : "false"));
		}
		rv--;	// Remove the last uneeded ',' of the above array
		BUF_ADD("],\"nics\":[");
		for (i = 0; i < (int)ARRAY_SIZE(N->nics); i++) {
			const struct sb_nics_conf *E = &N->nics[i];
			BUF_ADD("{\"nicID\":\"0x%016x%016x\",\"protocol\":\"%s\",\"guid\":\"0x%016x%016x\",\"pkey\":65535,\"version\":1,\"uuid\":\"" UUID_from_U32 "\"},",
				0xeee000, E->uuid, E->protocol, 0xeee111, E->uuid, E->uuid);
		}
		rv--;	// Remove the last uneeded ',' of the above array
		BUF_ADD("]},");		// Close nics array ']', node '}'
	}
	rv--;	// Remove the last uneeded ',' of the above array
	BUF_ADD("]}}");			// Close targets array, payload and json
	sim_broker_topic_msg_produce(g_mgmt_sim->k_producers.hw, buf, rv, false);
}

static void __handle_low_prio_msg(const rd_kafka_message_t *msg) {
	struct mm_json_elem *root = parse_json_txt_into_kv_tree(msg->payload, msg->len);
	const char *message_type = json_get_dict_str(root, "messageType", NULL);
	const uint64_t msg_seq = json_get_dict_num(root, "messageSequence", ~0UL);
	BUG_ON(!root || (root->type != JSON_E_DICT) || !message_type);
	if (strcmp(message_type, "driveZeroingProgress") == 0) {
		struct mm_json_elem *payload = json_get_dict_value(root, "payload");
		struct mgmt_sim_disk_status *d = __lookup_disk_by_uuid(json_get_dict_str(payload, "diskUUID", NULL));
		// const int n_zeroed_blocks = json_get_dict_num(payload, "nZeroedBlks", -1);
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
	struct mgmt_sim_state *m = g_mgmt_sim;
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
			unsigned uuid_u32 = 0;
			BUG_ON(sscanf(praid_uuid, "%x", &uuid_u32) != 1);	// Scan 1 argument
			if (m->cfg->vols[1].chunks[0].raids[0].uuid == uuid_u32) {
				m->v_r1_seg_zeroing_progress_seen = true;
				N_IMf(msim_szp, "segmentZeroingProgress: @STR praid matched", m->cfg->vols[1].name);
			}
	}
	nvmeibt_mm_json_free_kv_tree(root);
}

void mgmt_sim_destroy(bool do_verify_used) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	if (do_verify_used)
		BUG_ON((m->n_leader_keep_alives <= 0) || (m->raftTerm == 0));
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
		const char *disk_name = json_get_dict_str(disk_elem, "diskID", NULL);
		struct mgmt_sim_disk_status *d = __lookup_disk_by_name(disk_name);
		BUG_ON(!disk_elem || (disk_elem->type != JSON_E_DICT) || !disk_name);
		if (!d) continue;		// Ignore stock drivers

		nvmeibt_strlcpy(d->status, json_get_dict_str(disk_elem, "status", "unknown"), sizeof(d->status));
		d->format.counter_toma_reply_done =        (unsigned)json_get_dict_num(disk_elem, "formatRequestCounter", -1);
		d->format.counter_toma_reply_in_progress = (unsigned)json_get_dict_num(disk_elem, "activeFormatRequestCounter", -1);
		d->block_size = json_get_dict_num(disk_elem, "block_size", -1);
		d->metadata_size = json_get_dict_num(disk_elem, "metadata_size", -1);
		N_Tf(msim_disk, "disk=@STR status=@STR frc=@INT afrc=@INT, @UINT+@UINT[b]", disk_name, d->status, d->format.counter_toma_reply_done, d->format.counter_toma_reply_in_progress, d->block_size, d->metadata_size);
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
	struct mgmt_sim_state *m = g_mgmt_sim;
	bool v_r1_found = false;
	BUG_ON(!praids_update || (praids_update->type != JSON_E_ARRAY));
	for (int i = 0; i < praids_update->array.len; i++) {
		struct mm_json_elem *entry = praids_update->array.elements[i];
		const char *uuid = json_get_dict_str(entry, "uuid", NULL);
		unsigned uuid_u32 = 0;
		BUG_ON(!entry || (entry->type != JSON_E_DICT) || !uuid);
		BUG_ON(sscanf(uuid, "%x", &uuid_u32) != 1);	// Scan 1 argument
		if (m->cfg->vols[1].chunks[0].raids[0].uuid == uuid_u32) {
			const struct sb_volume_conf* V = &m->cfg->vols[1];
			struct mm_json_elem *segments = json_get_dict_value(entry, "segments");
			N_IMf(msim_praid, "matched @STR pRaid UUID", V->name);
			m->v_r1_praid_reported = true;
			v_r1_found = true;

			/* Check if all segments have status "deprecated" */
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
					m->v_r1_praid_deprecated = true;
					N_IMf(msim_praid_dep, "@STR all segments deprecated", V->name);
				}
			}
		}
	}
	/* After deleteVolumeCompleted: if V[1] praid is absent, it was garbage collected */
	if (m->v_r1_delete_completed_sent && !v_r1_found) {
		m->v_r1_praid_absent_from_report = true;
		N_IMf(msim_praid_gc, "@STR praid absent (garbage collected)",  m->cfg->vols[1].name);
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
	return __disk_ready_for_format(&m->disks_st[0]) && __disk_ready_for_format(&m->disks_st[1]);
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
void mgmt_sim_send_format_drive(int disk_idx) {
	struct mgmt_sim_disk_status *d = &g_mgmt_sim->disks_st[disk_idx];
	BUG_ON(__is_disk_fmt_running(d->format.state));
	d->format.counter_sent++;
	d->format.state = FMT_SENT;
	__send_format_drive_msg(d);
}

bool mgmt_sim_drive_format_is_done(int disk_idx) {
	struct mgmt_sim_disk_status *d = &g_mgmt_sim->disks_st[disk_idx];
	BUG_ON(d->format.state == FMT_IDLE); // should only be called after sending a format command
	return d->format.state == FMT_DONE;
}

void mgmt_sim_send_leader_keep_alive(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *payload = malloc(256);
	const size_t len = make_msg_update_leader_keepalive_token(payload, 256);
	sim_broker_topic_msg_produce(m->k_producers.l_vol, payload, len, false);
}

void mgmt_sim_send_add_volume_remote1(void)         { __send_msg_volume_add(0); }
void mgmt_sim_send_add_volume_r1(     void)         { __send_msg_volume_add(1); }

void mgmt_sim_send_delete_volume_r1(  void)         { __send_msg_volume_del(1, false); }
void mgmt_sim_send_delete_volume_completed_r1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	__send_msg_volume_del(1, true);
	m->v_r1_delete_completed_sent = true;
}
