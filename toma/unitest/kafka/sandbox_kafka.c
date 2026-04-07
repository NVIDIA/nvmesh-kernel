/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/* Implements all rd_kafka_* functions that production Toma code calls */
#include "sandbox_kafka_internal.h"	// Module interface headers
#include "sandbox_kafka_public.h"
#include "nvmeibt_debug.h"

/************************************* Internal struct definitions ********************************/
struct sim_broker_topic {		// Kafka Broker topic implementation = append-only log of messages
	pthread_mutex_t lock;		// Toma sends sends/consume messages only from kafka thread. Simulated management may send/consume in other thread
	enum sim_topic_type_toma_to_mgmt type;
	struct sim_msg {			// A single message stored in a topic log
		char *payload;
		size_t len;
	} *msgs;					// Circular buffer storing non commited messages (may be read by other side)
	uint32_t capacity;			// Allocated size of circular buffer. Defined during constructor, Typically use power of 2
	uint32_t n_msgs;			// Number of stored messages in the queue. n_msgs <= capacity
	int64_t committed_offset;	// Last committed offset msgs[committed_offset % capacity] was consumed, acked and deleted
	int64_t cur_offset;			// Offset of next message to read. msgs[cur_offset % capacity].  cur_offset > committed_offset!
	int64_t debug_highest_offset_ever_reached;	// Just for debug, kafka does not have it. For strict Verification of user behavior. = max(cur_offset)
	// Used buffer slots: [ (committed_offset+1)%capacity .. cur_offset%capacity  .. (committed_offset+msgs)%capacity ), All the rest have ->payload = NULL
	// Note: cur_offset belongs to client consumer not of broker. We have only 1 consumer so for simplicity and easy of debug, put it here
	struct error_inject_t {
		int next_msg_delta_offset;
	} err_inj;
};

static inline int64_t sim_broker_topic_get_msg_offset_last( const struct sim_broker_topic *t) { return t->committed_offset + t->n_msgs; }	// Offset of last message. The next to be produced message will be in end + 1
static inline int64_t sim_broker_topic_get_msg_offset_first(const struct sim_broker_topic *t) { return t->committed_offset + 1; }	// Assuming at least 1 message is inside
static inline void    sim_broker_topic_reset_to_earliest(         struct sim_broker_topic *t) {        t->cur_offset = sim_broker_topic_get_msg_offset_first(t); }

void sim_broker_topic_create(struct sim_broker_topic *t, enum sim_topic_type_toma_to_mgmt type, int max_queue_size) {
	pthread_mutex_init(&t->lock, NULL);
	t->type = type;
	t->capacity = max_queue_size;
	t->msgs = (struct sim_msg*)calloc(t->capacity, sizeof(struct sim_msg));
	t->committed_offset = -1;
	t->debug_highest_offset_ever_reached = t->cur_offset = 0;
	t->n_msgs = 0;
}

void sim_broker_topic_destroy(struct sim_broker_topic *t) {
	for (int64_t i = sim_broker_topic_get_msg_offset_first(t); i <= sim_broker_topic_get_msg_offset_last(t); i++)
		free(t->msgs[i % t->capacity].payload);		// Can use //for (uint32_t i = 0; i < t->capacity; i++) free(t->msgs[i].payload);
	free(t->msgs);
}

#define B_TYPE "KBROKER@CHAR_K."
void sim_broker_topic_msg_produce(struct sim_broker_topic *t, void *payload, size_t len, const bool should_copy) {
	struct sim_msg *m;
	uint32_t i;
	BUG_ON(!payload || !len);				// Wrong input. Must be a valid message
	BUG_ON(pthread_mutex_lock(&t->lock) != 0);
	BUG_ON(t->n_msgs >= t->capacity);		// Someone is not consuming the messages from the queue
	t->n_msgs++;
	i = sim_broker_topic_get_msg_offset_last(t) % t->capacity; // Slot where message goes
	m = &t->msgs[i];
	BUG_ON(m->payload || m->len);			// Circular buffer overrun or commited message not freed
	if (should_copy){
		m->payload = (char*)malloc(len + 1);
		memcpy(m->payload, payload, len);
		m->payload[len] = '\0';				// NULL-terminate for convenience
	} else {
		m->payload = payload;
	}
	m->len = len;
	N_Tf(__AUTOID__, B_TYPE "@KAFKA_OFST, slot[@INT]", t->type, sim_broker_topic_get_msg_offset_last(t), i);
	BUG_ON(pthread_mutex_unlock(&t->lock) != 0);
}
void sim_broker_topic_msg_inject_next_msg_offset(struct sim_broker_topic *t, int delta_offset) {
	BUG_ON(t->err_inj.next_msg_delta_offset);		// Previous injection did not happen
	t->err_inj.next_msg_delta_offset = delta_offset;
}


bool sim_broker_topic_msg_consume(struct sim_broker_topic *t, rd_kafka_message_t *rv) {	// Get current message
	rv->payload = NULL;											// If no message in queue, preinitialize to NULL
	BUG_ON(pthread_mutex_lock(&t->lock) != 0);
	if (t->n_msgs && (t->cur_offset <= sim_broker_topic_get_msg_offset_last(t))) {
		const uint32_t i = (uint32_t)(t->cur_offset % t->capacity);		// Slot where current message resides
		struct sim_msg *m = &t->msgs[i];
		BUG_ON(!m->payload);					// Valid message should exist in the queue
		rv->len = m->len;						// Just reference, Kafka simu owns the memory
		rv->payload = m->payload;				// Pointer to buffer in queue. Will remain valid until msg is commited
		rv->offset = t->cur_offset++;
		if (t->err_inj.next_msg_delta_offset) {
			N_Tf(__AUTOID__, B_TYPE "@KAFKA_OFST+@INT, slot[@INT]", t->type, rv->offset, t->err_inj.next_msg_delta_offset, i);
			rv->offset += t->err_inj.next_msg_delta_offset;
			t->err_inj.next_msg_delta_offset = 0;
		} else {
			N_Tf(__AUTOID__, B_TYPE "@KAFKA_OFST, slot[@INT]", t->type, rv->offset, i);
		}
		MAX_WITH(t->debug_highest_offset_ever_reached, t->cur_offset);
	} else { /* No message at this offset */}
	BUG_ON(pthread_mutex_unlock(&t->lock) != 0);
	rv->err = RD_KAFKA_RESP_ERR_NO_ERROR;
	return (rv->payload != NULL);
}

void sim_broker_topic_ack_offsets(struct sim_broker_topic *t, int64_t ack_offset) {	// Ack that consumer is done with this offset and all which are smaller
	int64_t i = -1, prev_committed;
	BUG_ON(pthread_mutex_lock(&t->lock) != 0);
	prev_committed = t->committed_offset;
	if ((ack_offset > sim_broker_topic_get_msg_offset_last(t)) || (ack_offset < 0)) {
		BUG_ON(true); return;						// Wrong argument! This message does not exists in kafka queue
	} else if (ack_offset == t->committed_offset) {
		goto _out;									// Already commited, OK and just do nothing
	} else if (ack_offset < t->committed_offset) {
		BUG_ON(true); return;						// Rewinding committed offset is valid in kafka but a very bad idea
	} else if (ack_offset >= t->debug_highest_offset_ever_reached) {
		BUG_ON(true); return;						// Msg was not read yet. How is it being committed
	}
	for (i = t->committed_offset + 1; i <= ack_offset; i++) {		// In kafka, commit is actually for this message and before
		struct sim_msg *m = &t->msgs[i % t->capacity];
		BUG_ON(!m->payload);						// Valid message should exist in the queue
		free(m->payload);
		m->payload = NULL;
		m->len = 0;
		t->n_msgs--;
	}
	t->committed_offset = ack_offset;
	if (t->committed_offset >= t->cur_offset)		// Msg was N read, cur moved back (to N-x) and now msg N commited. Real kafka does not move cur_offset, but upon restart it will move it to earliest
		sim_broker_topic_reset_to_earliest(t);		// Implemented not like kafka: We move cur to earliest immediately because we free commited messages
	N_Tf(__AUTOID__, B_TYPE "commited:@KAFKA_OFST -> @KAFKA_OFST, cur_@KAFKA_OFST, last_slot[@INT]", t->type, prev_committed, ack_offset, t->cur_offset, (int)(i % t->capacity));
	BUG_ON((t->n_msgs == 0) && (t->type == KTOPIC_TYPE_M2T_HW_CFG));		// Hardware configuration should always exist. This queue must never be empty
 _out:
	BUG_ON(pthread_mutex_unlock(&t->lock) != 0);
}

bool sim_broker_topic_is_empty(const struct sim_broker_topic *t) {
	return (t->n_msgs == 0);				// Todo, consider using mutex if non atomic access
}

struct rd_kafka_topic_conf_s { int dummy; };	// Unused by Toma

struct rd_kafka_topic_s {					// Kafka client topic emulation
	char *name;
	struct sim_broker_topic *broker_topic;	// Connection to broker (when topic is initialized)
	struct rd_kafka_topic_conf_s *conf;		// Might be NULL
	int32_t partition;						// Support only 1 partition for now. Store its index, always 0
	enum sim_topic_type_toma_to_mgmt type;	// String name is unique but its comparison is slow, so use this one.
	bool is_assigned;						// Todo: may remove it and set partition as -1 instead. User can read/write this topic (it has assigned 1 or more partitions)
};

struct rd_kafka_conf_s {
	char *group_id;
	bool enable_ssl;
	bool auto_reset_earliest;	// Represents: "auto.offset.reset",	"earliest"
};

struct rd_kafka_s {							// Kafka producer/consumer object
	char *name;
	int log_lvl;
	enum rd_kafka_type_t who;
	struct rd_kafka_conf_s *conf;
	struct rd_kafka_topic_s topic;			// We support only 1 topic per consumer/producer
};

struct kafka_simulator_t {
	struct sim_broker_topic topics[7];		// Kafka broker (backend) topics, always exist even if Toma is not connected to them via kafka client
	rd_kafka_t *obj[7];						// Kafka client: 4 Toma consumers, 3 Toma producers
	int n_obj;
	void (*notify_toma_producer_msg_accepted)( rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque);
	void (*notify_toma_consumer_offset_commit)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque);
	void (*notify_mgmt_simu_toma_send_msg)(struct sim_broker_topic *t);
};

/************************************* Module state ********************************/
static struct kafka_simulator_t *g_kafka_simu = NULL;

struct kafka_simulator_t *sandbox_kafka_init(void (*fn)(struct sim_broker_topic *t)) {
	struct kafka_simulator_t *ks = g_kafka_simu = calloc(1, sizeof(*g_kafka_simu));
	sim_broker_topic_create(&ks->topics[0], KTOPIC_TYPE_M2T_HW_CFG,			4);		// This queue is always non empty, stores at least the last hardware config
	sim_broker_topic_create(&ks->topics[1], KTOPIC_TYPE_M2T_CMD,			4);		// Toma will consume commands very fast, extra room for format re-sends
	sim_broker_topic_create(&ks->topics[2], KTOPIC_TYPE_M2T_TARGETS_RAFT,	8);		// This queue might be long and potentially store the entire history.
	sim_broker_topic_create(&ks->topics[3], KTOPIC_TYPE_M2T_VOLUMES,		4);		// Toma will consume volume commands very fast, and ack mgmt keepalive to leader also almost immediately
	sim_broker_topic_create(&ks->topics[4], KTOPIC_TYPE_T2M_PRIORITY,		1);		// Mgmt Simu will consume toma reports immediately
	sim_broker_topic_create(&ks->topics[5], KTOPIC_TYPE_T2M_KEEPALIVE,		1);		// Mgmt Simu will consume toma reports immediately, May discard all messages except for last one
	sim_broker_topic_create(&ks->topics[6], KTOPIC_TYPE_T2M_LOW,			1);		// Mgmt Simu will consume toma reports immediately
	ks->notify_mgmt_simu_toma_send_msg = fn;
	return ks;
}

void sandbox_kafka_destroy(struct kafka_simulator_t *ks) {
	BUG_ON(ks != g_kafka_simu);
	for (int i = 0; i < (int)ARRAY_SIZE(ks->topics); i++) {
		sim_broker_topic_destroy(&ks->topics[i]);
	}
	free(g_kafka_simu);
	g_kafka_simu = NULL;
}

/************************************* Static helpers ********************************/
static bool is_kafka_cp_used(const rd_kafka_t* o) {
	return (o->name != NULL);
}

static rd_kafka_t* kafka_simu_find_next_unused(struct kafka_simulator_t *ks) {
	rd_kafka_t *k;
	int i;
	for (i = 0; i < ks->n_obj; i++) {		// Reuse deleted
		if (ks->obj[i] == NULL)
			ks->obj[i] = calloc(1, sizeof(*k));
		if (!is_kafka_cp_used(ks->obj[i]))
			return ks->obj[i];
	}
	k = ks->obj[ks->n_obj++] = calloc(1, sizeof(*k));
	BUG_ON(i >= ARRAY_SIZE(ks->obj) || is_kafka_cp_used(k));
	return k;
}

static rd_kafka_t* kafka_simu_find_by_parition_name(const char* name) {
	struct kafka_simulator_t *ks = g_kafka_simu;
	int i;
	for (i = 0; i < ks->n_obj; i++) {
		rd_kafka_t *k = ks->obj[i];
		if (is_kafka_cp_used(k) && k->topic.name && !strcmp(k->topic.name, name))
			return k;
	}
	return NULL;
}

struct sim_broker_topic *sim_broker_topic_find_by(enum sim_topic_type_toma_to_mgmt type) {
	struct kafka_simulator_t *ks = g_kafka_simu;
	for (int i = 0; i < (int)ARRAY_SIZE(ks->topics); i++) {
		if (ks->topics[i].type == type)
			return &ks->topics[i];
	}
	BUG_ON(true); return NULL;
}

/************************************* Kafka API implementations ********************************/
rd_kafka_topic_conf_t* rd_kafka_topic_conf_new(void) {
	return calloc(1, sizeof(rd_kafka_topic_conf_t));
}
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *conf) { free(conf); }

const char* rd_kafka_topic_name(const rd_kafka_topic_t *kt) {
	return kt->name;
}

void rd_kafka_topic_destroy(rd_kafka_topic_t *kt) {
	if (kt->conf) {
		rd_kafka_topic_conf_destroy(kt->conf);
		kt->conf = NULL;
	}
	free(kt->name);
	memset(kt, 0, sizeof(*kt));
}

#define ASSIGN_FMT B_TYPE "committed_@KAFKA_OFST, cur_@KAFKA_OFST"
#define ASSIGN_ARG(bt) bt->type, bt->committed_offset, bt->cur_offset
rd_kafka_resp_err_t rd_kafka_assign(rd_kafka_t *ko, const rd_kafka_topic_partition_list_t *pl) {
	rd_kafka_topic_t *kt = &ko->topic;
	struct sim_broker_topic *bt = kt->broker_topic;
	BUG_ON(ko->who == RD_KAFKA_PRODUCER);
	N_Tf(__AUTOID__, "k_object=@STR, has_pl=@BOOL_YN", ko->name, !!pl);
	if (pl == NULL) {
		if (ko->topic.name && ko->topic.is_assigned) {
			N_Tf(__AUTOID__, ASSIGN_FMT "->0, Stop: @STR", ASSIGN_ARG(bt), kt->name);
			if (ko->conf->auto_reset_earliest)
				sim_broker_topic_reset_to_earliest(bt);
			kt->is_assigned = false;
		} // else: Topic was never created, or assign NULL called twice, this is valid
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	} else {
		const int64_t offset = pl->elems[0].offset;
		if (!pl->elems[0].k) {
			((rd_kafka_topic_partition_t*)&pl->elems[0])->k = ko;
			rd_kafka_topic_new(ko, pl->elems[0].topic, NULL);
		}
		bt = kt->broker_topic;
		BUG_ON((ko != pl->elems[0].k) || (kt->partition != pl->elems[0].partition));		// We dont support partitions
		N_Tf(__AUTOID__, ASSIGN_FMT ": start topic consume @STR from partition[@INT].@KAFKA_OFST", ASSIGN_ARG(bt), kt->name, kt->partition, offset);
		kt->is_assigned = true;
		if (offset == RD_KAFKA_OFFSET_STORED) {
			N_Tf(__AUTOID__, ASSIGN_FMT " Continue from stored", ASSIGN_ARG(bt)); // Toma relies on Kafka simulator
		} else {
			if (offset == RD_KAFKA_OFFSET_BEGINNING) {
				sim_broker_topic_reset_to_earliest(bt);
			} else {
				const bool is_OK_to_loose_msgs = ((bt->type == KTOPIC_TYPE_M2T_TARGETS_RAFT) || (bt->type == KTOPIC_TYPE_M2T_HW_CFG));	// Temp: Config will be re-sent again by mgmt, raft targets are in persistency so not needed
				const bool is_loading_kafka_from_persist = (offset > bt->debug_highest_offset_ever_reached);		// Toma intends to skip messages it never read, ie - it read them in previous run and saved offset to persistency
				BUG_ON((offset < 0) || (offset <= bt->committed_offset));	// Those messages do not exist in kafka queue
				bt->cur_offset = offset;	// Toma explicitly asks to start from a specific offset (taken from its RAM upon kafka soft init, or from persistency upon toma init or leader change).
				if (bt->n_msgs == 0) {
					bt->committed_offset = bt->cur_offset - 1;	// Simulate as if everything from toma persistency was in the past and now unitest is injecting new messages. No need to ack offsets as broker is empty
				} else if (is_loading_kafka_from_persist && (offset > bt->committed_offset) && !is_OK_to_loose_msgs) {		// Toma read this value from persistency. Going to skip messages in kafka queue. Why? Our kafka broker does not have persistency between runs but Toma does
					BUG_ON(bt->n_msgs != 0); // Otherwise toma will not read the messages pushed by unitest environment aand tests will break
				}
			}
			N_Tf(__AUTOID__, ASSIGN_FMT " n_msgs=@INT, CurSet", ASSIGN_ARG(bt), bt->n_msgs);
		}
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	}
}

rd_kafka_resp_err_t rd_kafka_assignment(rd_kafka_t *ko, rd_kafka_topic_partition_list_t **pl) {
	*pl = NULL;
	if (!ko->topic.is_assigned)
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	return RD_KAFKA_RESP_ERR__RETRY;		// Not implemented yet
}

rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk) {
	BUG_ON(rk->topic.is_assigned);	// Should destroy partition before. This is not a must according to kafka documentation but enforces a cleaner api
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t* me) { (void)me; return RD_KAFKA_RESP_ERR_NO_ERROR; }
const char*         rd_kafka_name(   const rd_kafka_t* me) { return me->name; }
void                rd_kafka_set_log_level(rd_kafka_t* me, int lvl) { me->log_lvl = lvl; }
rd_kafka_resp_err_t rd_kafka_flush(rd_kafka_t *rk, int timeout_ms) { (void)rk; (void)timeout_ms; return RD_KAFKA_RESP_ERR_NO_ERROR; }
int                 rd_kafka_poll(         rd_kafka_t* me, bool is_blocking) { (void)me; (void)is_blocking; return 0; }
rd_kafka_resp_err_t rd_kafka_commit(rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int is_async) {
	BUG_ON(me != pl->elems[0].k);
	sim_broker_topic_ack_offsets(me->topic.broker_topic, (pl->elems[0].offset - 1) /*last_consumed*/);
	(void)is_async;
	g_kafka_simu->notify_toma_consumer_offset_commit(me, RD_KAFKA_RESP_ERR_NO_ERROR, pl, NULL);
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_committed(rd_kafka_t *me, rd_kafka_topic_partition_list_t *pl, int timeout_ms) {
	BUG_ON(me != pl->elems[0].k);
	BUG_ON(timeout_ms < 1000);
	pl->elems[0].offset = sim_broker_topic_get_msg_offset_first(me->topic.broker_topic);
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}
char* rd_kafka_err2str( rd_kafka_resp_err_t e) { return e ? "kerr2do" : "OK"; }
char* rd_kafka_err2name(rd_kafka_resp_err_t e) { return e ? "kerr2do" : "OK"; }
rd_kafka_resp_err_t rd_kafka_last_error(void) { return RD_KAFKA_RESP_ERR_NO_ERROR; }
rd_kafka_conf_t* rd_kafka_conf_new(void) { return calloc(1, sizeof(rd_kafka_conf_t)); }
void rd_kafka_conf_destroy(rd_kafka_conf_t* me) { free(me); }
void rd_kafka_message_destroy(rd_kafka_message_t *msg) { free(msg); /* msg->payload is a pointer to broker buffer, will auto-free upon msg commit. Dont touch it*/ }
void rd_kafka_conf_set_error_cb( rd_kafka_conf_t *kc, void (*fn)(rd_kafka_t *rk, int err, const char *reason, void *opaque)) { (void)kc; (void)fn; }

void rd_kafka_destroy(rd_kafka_t* k) {
	struct kafka_simulator_t *ks = g_kafka_simu;
	int i;
	for (i = 0; i < ks->n_obj; i++) {
		if (ks->obj[i] == k) {
			free(k->name);
			free(k->conf);
			rd_kafka_topic_destroy(&k->topic);
			free(k);
			ks->obj[i] = NULL;
			while ((ks->n_obj > 0) && (ks->obj[ks->n_obj-1] == NULL))		// Shrink the array
				ks->n_obj--;
			return;
		}
	}
	BUG_ON(true);
}

rd_kafka_t* rd_kafka_new(enum rd_kafka_type_t who, rd_kafka_conf_t *cfg, char*err_str, size_t size_of_err) {
	struct kafka_simulator_t *ks = g_kafka_simu;
	rd_kafka_t *k = kafka_simu_find_next_unused(ks);
	if (who == RD_KAFKA_CONSUMER) {} else {	/* RD_KAFKA_PRODUCER */	}
	k->conf = cfg;
	k->name = cfg->group_id;
	k->who = who;
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return k;
}

rd_kafka_topic_t* rd_kafka_topic_new(rd_kafka_t *k, const char *name, rd_kafka_topic_conf_t *conf) {
	rd_kafka_topic_t *kt = &k->topic;
	BUG_ON(!is_kafka_cp_used(k) || (kt->name != NULL) || (kt->is_assigned));
	kt->name = strdup(name);
	kt->conf = conf;
	kt->partition = 0;
	if (k->who == RD_KAFKA_PRODUCER) {
		if (     strstr(name, "management.priority."))		kt->type = KTOPIC_TYPE_T2M_PRIORITY;
		else if (strstr(name, "management.keepalive."))		kt->type = KTOPIC_TYPE_T2M_KEEPALIVE;
		else if (strstr(name, "management.low."))			kt->type = KTOPIC_TYPE_T2M_LOW;
		else BUG_ON(true);				// Unknown topic which management simulator will not listen too
	} else {
		if (     strstr(name, "hardwareConfiguration"))		kt->type = KTOPIC_TYPE_M2T_HW_CFG;
		else if (strstr(name, "TOMA.commands."))			kt->type = KTOPIC_TYPE_M2T_CMD;
		else if (strstr(name, "TargetUpdates"))				kt->type = KTOPIC_TYPE_M2T_TARGETS_RAFT;
		else if (strstr(name, "incrementalUpdates"))		kt->type = KTOPIC_TYPE_M2T_VOLUMES;
		else BUG_ON(true);				// Unknown topic which Toma will not listen too
	}
	kt->broker_topic = sim_broker_topic_find_by(kt->type);
	N_Tf(__AUTOID__, "@STR: alloc new topic @STR -> " B_TYPE "cur_@KAFKA_OFST", k->name, kt->name, kt->type, kt->broker_topic->cur_offset);
	return kt;
}

rd_kafka_topic_partition_list_t* rd_kafka_topic_partition_list_new(int n) {
	rd_kafka_topic_partition_list_t* rv = calloc(1, sizeof(rd_kafka_topic_partition_list_t));
	BUG_ON(n != 1);			//  1 element of partition=0
	rv->size = sizeof(*rv);
	return rv;
}

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add(rd_kafka_topic_partition_list_t *pl, const char* name, int32_t partition) {
	rd_kafka_topic_partition_t *p = &pl->elems[0];
	pl->cnt++;
	BUG_ON((pl->cnt != 1) || (partition != 0));	// Our implementation of partition list has only 1 element of partition=0. Do not allow calling add twice
	p->k = kafka_simu_find_by_parition_name(name);	// Store pointer to 'k' for future retrieval. Can be NULL (topic was not created yet, will auto-create when pl is assigned)
	p->partition = partition;
	p->offset = RD_KAFKA_OFFSET_INVALID;
	p->topic = name;
	return p;
}

void rd_kafka_topic_partition_list_destroy(rd_kafka_topic_partition_list_t* pl) {
	free(pl);
}

rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *me, const char *str, int32_t partition, int64_t *low_oldest_beginning_offset, int64_t *high_newest_end_offset, int timeout) {
	BUG_ON(strcmp(me->topic.name, str) || (me->topic.partition != partition));	// Only 1 partition
	(void)timeout;
	*low_oldest_beginning_offset = sim_broker_topic_get_msg_offset_first(me->topic.broker_topic);
	*high_newest_end_offset =      sim_broker_topic_get_msg_offset_last( me->topic.broker_topic) + 1;
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_conf_set_dr_msg_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque)) {
	g_kafka_simu->notify_toma_producer_msg_accepted = fn;
	(void)kc;
}

void rd_kafka_conf_set_rebalance_cb(rd_kafka_conf_t* kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	(void)kc; (void)fn;
}
void rd_kafka_conf_set_offset_commit_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	g_kafka_simu->notify_toma_consumer_offset_commit = fn;
	(void)kc;
}

int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque) {
	static int fail_once_every = 0;							// Do per topic and not generic?
	rd_kafka_t *ko = container_of(kt, rd_kafka_t, topic);
	rd_kafka_message_t km;
	km._private = msg_opaque;
	km.err = (fail_once_every++ % 3) ? 0 : RD_KAFKA_RESP_ERR__TIMED_OUT;		// Once every few messages fail completion
	BUG_ON((partition != RD_KAFKA_PARTITION_UA) || (len == 0) || ((key == NULL) != (keylen == 0)));
	sim_broker_topic_msg_produce(ko->topic.broker_topic, payload, len, (msgflags & RD_KAFKA_MSG_F_COPY));
	g_kafka_simu->notify_toma_producer_msg_accepted(ko, &km, NULL);
	g_kafka_simu->notify_mgmt_simu_toma_send_msg(ko->topic.broker_topic);
	errno = 0;
	return 0;
}

rd_kafka_resp_err_t rd_kafka_fatal_error(rd_kafka_t *k, char *errstr, size_t errstr_size) {
	(void)k; (void)errstr_size;
	errstr[0] = 0;
	return RD_KAFKA_RESP_ERR_NO_ERROR;	// Or RD_KAFKA_RESP_ERR__FATAL???
}

rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t *kc, const char *key, const char *val, char* err_str, size_t size_of_err) {
	BUG_ON(!kc || !key || !val);
	if (!strcmp(key, "group.id") || !strcmp(key, "client.id")) {
		if (!kc->group_id)
			kc->group_id = strdup(val);
	} else if (strstr(key, "ssl.") != 0) {
		kc->enable_ssl = true;
	} else if (strstr(key, "auto.offset.reset") != 0) {
		BUG_ON(val[0] != 'e');				// Verify this is earliest
		kc->auto_reset_earliest = true;
	}
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return RD_KAFKA_CONF_OK;
}

rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *ko, int timeout_ms) {
	rd_kafka_message_t *m = calloc(1, sizeof(*m));
	struct sim_broker_topic *t = ko->topic.broker_topic;
	BUG_ON((timeout_ms != 0) || (!ko->topic.is_assigned));
	if (sim_broker_topic_msg_consume(t, m)) {
		m->_private = NULL;
		return m;
	}
	free(m);			// No message
	return NULL;
}
