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
	struct sb_disk_conf *conf;			// Configuration
	char status[16];					// As reported by Toma
	struct t_format_monitor {
		unsigned counter_sent;			// Ever increasing generation for for disk format cmd to toma. Value sent in last formatDrive
		unsigned counter_toma_reply_done;
		unsigned counter_toma_reply_in_progress;
		int      msg_seq;				// Msg's can arrive unordered and multiple times. Use sequence to discared already processed messages
		enum e_disk_format_state state;	// per-drive format tracking
	} format;
};

#define DISK_ID_FMT "\"diskID\":\"%s.%d\",\"uuid\":\"" UUID_from_U32 "\""		// Sends ID of 'struct sb_disk_conf *' to Toma
#define DISK_ID_VAL(D) (D)->serial, (D)->name_space_id, (D)->uuid
static int make_msg_format_drive(char *buf, size_t capacity, const struct mgmt_sim_disk_status *d, unsigned long boot_time) {
	return snprintf(buf, capacity,
		"{\"messageType\":\"formatDrive\",\"messageTypeVersion\":1,\"payload\":{" DISK_ID_FMT ",\"vendor\":%u"
		",\"formatType\":\"format_ec\",\"formatRequestCounter\":%u"
		",\"blockSize\":4096,\"metadataSize\":8,\"bootTime\":%lu"
		", " MGMT_DB_UUID_JSON "}}",
		DISK_ID_VAL(d->conf), d->conf->vendor, d->format.counter_sent, boot_time);
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

void mgmt_sim_send_add_volume(int vol_idx) {
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
	N_IMf(__AUTOID__, "vol=@DEV_NAME sending msg addVolume, @INT[b]", m->cfg->vols[vol_idx].name, rv);
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
	return &g_mgmt_sim->disks_st[sb_cluster_get_disk_idx_from_disk_name(g_mgmt_sim->cfg, drive_name)];
}

static struct mgmt_sim_disk_status *__lookup_disk_by_uuid(const char *disk_uuid) {
	return &g_mgmt_sim->disks_st[sb_cluster_get_disk_idx_from_disk_uuid(g_mgmt_sim->cfg, disk_uuid)];
}

static void __send_format_drive_msg(const struct mgmt_sim_disk_status *d) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(1024);
	size_t len = (size_t)make_msg_format_drive(buf, 1024, d, (unsigned long)m->boot_time);
	N_IMf(__AUTOID__, "sending formatDrive disk=@STR format_gen=@INT, bootTime=@INT64_TD", d->conf->serial, d->format.counter_sent, m->boot_time);
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
}

static void __check_format_progress(struct mgmt_sim_disk_status *d, int msg_seq, bool on_report_target_msg) {
	if (msg_seq <= d->format.msg_seq) {
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], old message, seq=@INT <= @INT, ignoring", d->conf->serial, d->format.state, d->format.state, msg_seq, d->format.msg_seq);
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
		N_Tf(__AUTOID__, "@STR.format_status[@CHAR->@CHAR], seq=@INT, format_gen=@INT, @STR[report]", d->conf->serial, prev_state, d->format.state, msg_seq, expected, on_report_target_msg ? "Target" : "Zeroin");
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
			const u32 fmt_counter = (n==0) ? m->disks_st[i].format.counter_sent : 1;	// Live toma formats disks, for simulated toma generation is irrelevant.
			BUF_ADD("{" DISK_ID_FMT ",\"blocks\":%u,\"block_size\":%u,\"activeFormatRequestCounter\":%u,\"vendorID\":%d,\"version\":7,\"isOutOfService\":%s},",
				DISK_ID_VAL(D), D->num_blocks, D->block_size, fmt_counter, D->vendor, (D->is_out_of_service ? "true" : "false"));
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
			// N_Tf(__AUTOID__, "@STR", msg->payload);
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

/******************************************************************************/
/* Static helper functions                                                    */
/******************************************************************************/
static void __extract_disks_status_from_report_target_msg(struct mm_json_elem *disks_array, uint64_t msg_seq) {
	int i;
	for (i = 0; i < disks_array->array.len; i++) {
		struct mm_json_elem *disk_elem = disks_array->array.elements[i];
		const char *disk_name = json_get_dict_str(disk_elem, "diskID", NULL);
		struct mgmt_sim_disk_status *d = __lookup_disk_by_name(disk_name);		// Here we could extract 'Serial_Number' which is like id but without name space
		struct sb_disk_conf *mdb = d->conf;
		const int nsid = (int)json_get_dict_num(disk_elem, "nsid", -1);
		BUG_ON(!disk_elem || (disk_elem->type != JSON_E_DICT));
		mdb->num_blocks =    json_get_dict_num(disk_elem, "blocks",        1);
		mdb->block_size =    json_get_dict_num(disk_elem, "block_size",    1);
		mdb->metadata_size = json_get_dict_num(disk_elem, "metadata_size", 1);
		sb_cluster_update_disk_vendor_and_verify(d->conf, json_get_dict_str(disk_elem, "Vendor", NULL));
		nvmeibt_strlcpy(d->status, json_get_dict_str(disk_elem, "status", "????"), sizeof(d->status));
		if (nsid == 1) {				// Ignore stock drivers for now
			sb_cluster_update_disk_namespace_from_name(d->conf, disk_name);
			d->format.counter_toma_reply_done =        (unsigned)json_get_dict_num(disk_elem, "formatRequestCounter", -1);
			d->format.counter_toma_reply_in_progress = (unsigned)json_get_dict_num(disk_elem, "activeFormatRequestCounter", -1);
			N_Tf(__AUTOID__, "disk=@STR @UINT+@UINT[b] status=@STR frc=@INT afrc=@INT", disk_name, mdb->block_size, mdb->metadata_size, d->status, d->format.counter_toma_reply_done, d->format.counter_toma_reply_in_progress);
			__check_format_progress(d, (int)msg_seq, true);
		} else {
			N_Tf(__AUTOID__, "disk=@STR @UINT+@UINT[b] status=@STR <<--- stock nvme!" , disk_name, mdb->block_size, mdb->metadata_size, d->status);
		}
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

static void __mongodb_insert_praid_hdr(struct sb_praid_topo *pr, struct mm_json_elem *j) {
	const int64_t pr_maj = json_get_dict_num(j, "pRaidMajorVersion", -1);
	const int64_t pr_min = json_get_dict_num(j, "pRaidMinorVersion", -1);
	BUG_ON(pr_maj < (int64_t)pr->version_major);						// Major version can never go back
	if (pr_min < (int64_t)pr->version_minor)
		BUG_ON(pr_maj <= (int64_t)pr->version_major);					// Minor can decrease only if major increases
	pr->version_major = pr_maj;
	pr->version_minor = pr_min;
}

static void __mongodb_insert_praid_seg(struct sb_cluster_conf *cfg, struct mm_json_elem *j) {
	const char *uuid = json_get_dict_str(j, "segmentID", NULL);
	const char *status = json_get_dict_str(j, "status",   "unknown");	// Generated with nvmeibt_mm_segment_persistent_status_to_str()
	const char *vital =  json_get_dict_str(j, "vitality", "unknown");
	struct sb_seg_topo *ps = sb_cluster_get_topo_seg_ptr_from_uuid(cfg, uuid);
	if      (!strncmp(vital, "up",   2))	ps->vitality = true;
	else if (!strncmp(vital, "down", 4))	ps->vitality = false;
	else BUG_ON(true);						// Unknown invalid value
	if      (!strncmp(status, "under_", 6))	ps->status = mdb_WRITE;		// under recovery
	else if (!strncmp(status, "normal", 6))	ps->status = mdb_seg_RW;
	else if (!strncmp(status, "deprec", 6))	ps->status = mdb_seg_dep;	// Deprecated
	else if (!strncmp(status, "dead",   4))	ps->status = mdb_DEAD;
	else if (!strncmp(status, "replac", 6))	ps->status = mdb_seg_rep;	// Replacement for deprecated
	else if (!strncmp(status, "conf_c", 6))	ps->status = mdb_seg_CORRUPTED;
	else if (!strncmp(status, "bootin", 6))	ps->status = mdb_seg_BOOT;
	else if (!strncmp(status, "zeroin", 6))	ps->status = mdb_seg_ZERO;
	else if (!strncmp(status, "initia", 6))	ps->status = mdb_seg_INIT;
	else BUG_ON(true);													// Unknown invalid value
}

static void mgmt_sim_parse_praid_report(struct mm_json_elem *root) {
	struct mm_json_elem *payload = json_get_dict_value(root, "payload");
	struct mm_json_elem *praids_update = json_get_dict_value(payload, "pRaidsUpdate");
	struct mgmt_sim_state *m = g_mgmt_sim;
	bool v_r1_found = false;
	BUG_ON(!praids_update || (praids_update->type != JSON_E_ARRAY));
	for (int i = 0; i < praids_update->array.len; i++) {
		struct mm_json_elem *entry = praids_update->array.elements[i];
		struct mm_json_elem *segments = json_get_dict_value(entry, "segments");
		const char *uuid = json_get_dict_str(entry, "uuid", NULL);
		struct sb_praid_topo *pr = sb_cluster_get_topo_prd_ptr_from_uuid(m->cfg, uuid);
		const int n_segs = pr->cfg->D + pr->cfg->P;
		BUG_ON(!segments || (segments->type != JSON_E_ARRAY) || (segments->array.len != n_segs));
		__mongodb_insert_praid_hdr(pr, entry);
		for (int j = 0; j < n_segs; j++)
			__mongodb_insert_praid_seg(m->cfg, segments->array.elements[j]);

		if (&m->cfg->vols[1].topo_chunks[0].raids[0] == pr) {
			const struct sb_volume_conf* V = &m->cfg->vols[1];
			bool all_deprecated = true;
			m->v_r1_praid_reported = true;
			v_r1_found = true;

			/* Check if all segments have status "deprecated" */
			for (int j = 0; (j < n_segs) && all_deprecated; j++)
				all_deprecated &= (pr->segs[j].status == mdb_seg_dep);
			if (all_deprecated) {
				m->v_r1_praid_deprecated = true;
				N_IMf(msim_praid_dep, "@STR all segments deprecated", V->name);
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

void mgmt_sim_send_praid_report_req(const u32 praid_uuid) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(512);
	size_t len = snprintf(buf, 512,
		"{\"messageType\":\"sendPRaidReport\",\"messageTypeVersion\":1,\"payload\":{\"pRaids\":[{\"uuid\":\"" UUID_from_U32 "\",\"lastKnownVersion\":\"<5,2,17>\"},{\"uuid\":\"" UUID_from_U32 "\",\"lastKnownVersion\":\"<6,1,12>\"}],\"bootTime\":%lu, " MGMT_DB_UUID_JSON "}}",
		praid_uuid, praid_uuid, m->boot_time);
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
}

void mgmt_sim_send_volume_exclusive_attach_notify(const u32 volume_uuid) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	char *buf = malloc(512);
	size_t len = snprintf(buf, 512,
		"{\"messageType\":\"reservationModeChange\",\"messageTypeVersion\":1,\"payload\":{\"volumeUUID\":\"" UUID_from_U32 "\",\"reservationMode\":\"Exclusive\",\"reservationVersion\":55656,\"bootTime\":%lu, " MGMT_DB_UUID_JSON "}}", volume_uuid, m->boot_time);
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
}

void mgmt_sim_send_disk_report_req(const u32 disk_idx) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	const struct mgmt_sim_disk_status *d = &m->disks_st[disk_idx];
	char *buf = malloc(512);
	size_t len = snprintf(buf, 512,
		"{\"messageType\":\"resendReport\",\"messageTypeVersion\":1,\"payload\":{\"drives\":[{" DISK_ID_FMT ",\"vendor\":%u,\"reappearingCounter\":789576,\"reappearingOutOfSync\":1}],\"bootTime\":%lu, " MGMT_DB_UUID_JSON "}}",
		DISK_ID_VAL(d->conf), d->conf->vendor, m->boot_time);
	// Todo: Inject field reappearingCounter, from incomming message segmentsDirtyBitsUpdate
	sim_broker_topic_msg_produce(m->k_producers.cmd, buf, len, false);
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

void mgmt_sim_send_delete_volume_r1(  void)         { __send_msg_volume_del(1, false); }
void mgmt_sim_send_delete_volume_completed_r1(void) {
	struct mgmt_sim_state *m = g_mgmt_sim;
	__send_msg_volume_del(1, true);
	m->v_r1_delete_completed_sent = true;
}
