/*
 * sandbox_kafka.c - Kafka simulator implementation for Toma sandbox
 *
 * Implements all rd_kafka_* functions that production Toma code calls.
 * The Kafka simulator state (kafka_simulator_t) is allocated here and
 * a pointer is stored in the global TSB struct for debugger visibility.
 */
#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module

// Module interface headers
#include "sandbox_kafka_internal.h"
#include "sandbox_kafka_public.h"

// Sandbox internal headers
#include "mgmt_sim.h"
#include "sandbox_util.h"

// NVMesh/Toma headers
#include "nvmeibt_debug.h"

// C Standard Library headers
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/************************************* Internal struct definitions ********************************/
struct rd_kafka_topic_conf_s {
	int dummy;
};

struct rd_kafka_topic_s {
	char *name;
	rd_kafka_topic_conf_t *conf;
	int64_t commited_offset, cur_offset;
	// Todo: Linked list of messages for offsets above cur,cur+1,....last_offset
	int32_t partition;		// Support only 1 partition for now. Store its index
	enum sim_topic_type_toma_to_mgmt type;	// string name is unique but its comparison is slow.
	bool is_active;
};

struct rd_kafka_conf_s {
	char *group_id;
	bool enable_ssl;
};

struct rd_kafka_s {
	char *name;
	int log_lvl;
	enum rd_kafka_type_t who;
	rd_kafka_conf_t *conf;
	struct rd_kafka_topic_s topic;
};

struct kafka_simulator_t {
	rd_kafka_t *obj[10];
	int n_obj;
	void (*notify_producer_msg_accepted)( rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque);
	void (*notify_consumer_offset_commit)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque);
};

/************************************* Module state ********************************/
static struct kafka_simulator_t *g_kafka_simu = NULL;

struct kafka_simulator_t *sandbox_kafka_init(void) {
	g_kafka_simu = calloc(1, sizeof(*g_kafka_simu));
	return g_kafka_simu;
}

void sandbox_kafka_destroy(struct kafka_simulator_t *ks) {
	BUG_ON(ks != g_kafka_simu);
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

static void __reset_offset(rd_kafka_topic_t *kt, int64_t offset) {
	BUG_ON(offset < 0);
	kt->commited_offset = offset - 1;
	kt->cur_offset = offset;
	N_Tf(__AUTOID__, "@STR: cur_offset=@LD", kt->name, kt->cur_offset);
}

static void __rd_kafka_topic_init(rd_kafka_topic_t *kt, const char* name, rd_kafka_topic_conf_t* conf) {
	BUG_ON((kt->name != NULL) || (kt->is_active));
	N_Tf(__AUTOID__, "@STR: alloc_init", name);
	kt->name = strdup(name);
	kt->conf = conf;
	__reset_offset(kt, 0);
	kt->partition = 0;
	kt->is_active = false;
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

rd_kafka_resp_err_t rd_kafka_assign(rd_kafka_t *ko, const rd_kafka_topic_partition_list_t *pl) {
	rd_kafka_topic_t *kt = &ko->topic;
	N_Tf(__AUTOID__, "k_object=@STR, has_pl=@BOOL_YN", ko->name, !!pl);
	if (pl == NULL) {
		if (ko->topic.name && ko->topic.is_active) {
			N_Tf(__AUTOID__, "@STR: stop. cur_offset=@LD", kt->name, kt->cur_offset);
			kt->is_active = false;
		} // Topic was never created
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	} else {
		const int64_t offset = pl->elems[0].offset;
		if (!pl->elems[0].k) {
			((rd_kafka_topic_partition_t*)&pl->elems[0])->k = ko;
			rd_kafka_topic_new(ko, pl->elems[0].topic, NULL);
		}
		BUG_ON((ko != pl->elems[0].k) || (kt->partition != pl->elems[0].partition));		// We dont support partitions
		N_Tf(__AUTOID__, "@STR: start topic consume from offset=@LD", kt->name, offset);
		kt->is_active = true;
		if (offset == RD_KAFKA_OFFSET_STORED) {
			N_Tf(__AUTOID__, "@STR: continue from cur_offset=@LD", kt->name, kt->cur_offset); // Toma relies on Kafka simulator
		} else if (offset == RD_KAFKA_OFFSET_BEGINNING) {
			__reset_offset(kt, 0);
		} else {	// Toma explicitly asks to start from a specific offset (taken from its RAM upon kafka soft init, or from persistency upon toma init orleader change).
			__reset_offset(kt, offset);
		}
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	}
}

rd_kafka_resp_err_t rd_kafka_assignment(rd_kafka_t *ko, rd_kafka_topic_partition_list_t **pl) {
	*pl = NULL;
	if (!ko->topic.is_active)
		return RD_KAFKA_RESP_ERR_NO_ERROR;
	return RD_KAFKA_RESP_ERR__RETRY;		// Not implemented yet
}

rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk) {
	rk->topic.is_active = false;
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t* me) { (void)me; return RD_KAFKA_RESP_ERR_NO_ERROR; }
const char*         rd_kafka_name(   const rd_kafka_t* me) { return me->name; }
void                rd_kafka_set_log_level(rd_kafka_t* me, int lvl) { me->log_lvl = lvl; }
rd_kafka_resp_err_t rd_kafka_flush(rd_kafka_t *rk, int timeout_ms) { (void)rk; (void)timeout_ms; return RD_KAFKA_RESP_ERR_NO_ERROR; }
int                 rd_kafka_poll(         rd_kafka_t* me, bool is_blocking) { (void)me; (void)is_blocking; return 0; }
rd_kafka_resp_err_t rd_kafka_commit(rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int is_async) {
	const int64_t last_consumed = (pl->elems[0].offset - 1);
	BUG_ON(me != pl->elems[0].k);
	BUG_ON((last_consumed >= me->topic.cur_offset));		// Todo: Maybe off by 1 here
	me->topic.commited_offset = max(last_consumed, me->topic.commited_offset);
	//SANDBOX_PRINT_TMP("%s: Commit %lu\n", me->topic.name, me->topic.commited_offset);
	(void)is_async;
	g_kafka_simu->notify_consumer_offset_commit(me, RD_KAFKA_RESP_ERR_NO_ERROR, pl, NULL);
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_committed(rd_kafka_t *me, rd_kafka_topic_partition_list_t *pl, int timeout_ms) {
	BUG_ON(me != pl->elems[0].k);
	BUG_ON(timeout_ms < 1000);
	pl->elems[0].offset = me->topic.cur_offset;
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}
char* rd_kafka_err2str(rd_kafka_resp_err_t e) { (void)e; return "kerr"; }
char* rd_kafka_err2name(rd_kafka_resp_err_t e) { (void)e; return "kerr"; }
rd_kafka_resp_err_t rd_kafka_last_error(void) { return RD_KAFKA_RESP_ERR_NO_ERROR; }
rd_kafka_conf_t* rd_kafka_conf_new(void) { return calloc(1, sizeof(rd_kafka_conf_t)); }
void rd_kafka_conf_destroy(rd_kafka_conf_t* me) { free(me); }
void rd_kafka_message_destroy(rd_kafka_message_t*msg) { free(msg->payload); free(msg); }
void rd_kafka_conf_set_error_cb( rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, int err, const char *reason, void *opaque)) { (void)kc; (void)fn; }

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
	BUG_ON(i >= ARRAY_SIZE(ks->obj) || (!k));
}

rd_kafka_t* rd_kafka_new(enum rd_kafka_type_t who, rd_kafka_conf_t *cfg, char*err_str, size_t size_of_err) {
	struct kafka_simulator_t *ks = g_kafka_simu;
	rd_kafka_t *k = kafka_simu_find_next_unused(ks);
	if (who == RD_KAFKA_CONSUMER) {
	} else {	// RD_KAFKA_PRODUCER
	}
	k->conf = cfg;
	k->name = cfg->group_id;
	k->who = who;
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return k;
}

rd_kafka_topic_t* rd_kafka_topic_new(rd_kafka_t *k, const char* name, rd_kafka_topic_conf_t* conf) {
	BUG_ON(!is_kafka_cp_used(k));
	__rd_kafka_topic_init(&k->topic, name, conf);
	k->topic.is_active = true;
	k->topic.type = KTOPIC_TYPE_T2M_UNKNOWN;
	if (k->who == RD_KAFKA_PRODUCER) {
		if (     strstr(name, "management.priority."))	k->topic.type = KTOPIC_TYPE_T2M_PRIORITY;
		else if (strstr(name, "management.keepalive."))	k->topic.type = KTOPIC_TYPE_T2M_KEEPALIVE;
		else if (strstr(name, "management.low."))		k->topic.type = KTOPIC_TYPE_T2M_LOW;
		else BUG_ON(true);				// unknown topic which management simulator will not listen too
	} else {
		N_Tf(__AUTOID__, "alloc new consumer topic @STR, starting from offset @LD", k->topic.name, k->topic.cur_offset);
	}
	return &k->topic;
}

rd_kafka_topic_partition_list_t* rd_kafka_topic_partition_list_new(int n) {
	rd_kafka_topic_partition_list_t* rv = calloc(1, sizeof(rd_kafka_topic_partition_list_t));
	BUG_ON(n != 1);			//  1 element of partition=0
	rv->size = sizeof(*rv);
	return rv;
}

rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add(rd_kafka_topic_partition_list_t *pl, const char* name, int32_t partition) {
	rd_kafka_topic_partition_t *p = &pl->elems[0];
	rd_kafka_t* k = kafka_simu_find_by_parition_name(name);
	pl->cnt++;
	BUG_ON((pl->cnt != 1) || (partition != 0));	// Our implementation of partition list has only 1 element of partition=0. Do not allow calling add twice
	p->k = k;									// Store pointer to 'k' for future retrieval. Can be NULL (topic was not created yet, will auto-create when pl is assigned)
	p->partition = partition;
	p->offset = RD_KAFKA_OFFSET_INVALID;
	p->topic = name;
	return p;
}

void rd_kafka_topic_partition_list_destroy(rd_kafka_topic_partition_list_t* pl) {
	free(pl);
}

rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *me, const char *str, int32_t partition, int64_t *low_oldest_beginning_offset, int64_t *high_newest_end_offset, int timeout) {
	BUG_ON(strcmp(me->topic.name, str));
	BUG_ON(me->topic.partition != partition);					// Only 1 partition
	(void)timeout;
	*low_oldest_beginning_offset = me->topic.cur_offset;
	*high_newest_end_offset =      me->topic.cur_offset + 17;		// +17 is just for fun, meaningless
	return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_conf_set_dr_msg_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque)) {
	g_kafka_simu->notify_producer_msg_accepted = fn;
	(void)kc;
}

void rd_kafka_conf_set_rebalance_cb(rd_kafka_conf_t* kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	(void)kc; (void)fn;
}
void rd_kafka_conf_set_offset_commit_cb(rd_kafka_conf_t*kc, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque)) {
	g_kafka_simu->notify_consumer_offset_commit = fn;
	(void)kc;
}

int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque) {
	static int fail_once_every = 0;
	rd_kafka_t *ko = container_of(kt, rd_kafka_t, topic);
	rd_kafka_message_t km;
	km._private = msg_opaque;
	km.err = (fail_once_every++ % 3) ? 0 : RD_KAFKA_RESP_ERR__TIMED_OUT;		// Once every few messages fail completion
	BUG_ON((partition != RD_KAFKA_PARTITION_UA) || (len == 0) || ((key == NULL) != (keylen == 0)));
	(void)msgflags;
	mgmt_sim_on_toma_produced(kt->type, payload, len);

	// No, put this on to kt, in a list and then poll_cb will return the callbacks
	g_kafka_simu->notify_producer_msg_accepted(ko, &km, NULL);
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
	}
	// SANDBOX_PRINT("KAFKA_SIMU::conf_set(): %p) %s=%s\n", kc, key, val);
	/* Set error 0 */ BUG_ON(size_of_err < 16); err_str[0] = 0;
	return RD_KAFKA_CONF_OK;
}

rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *ko, int timeout_ms) {
	rd_kafka_message_t *m = calloc(1, sizeof(*m));
	const char *unique_name = (ko->name[0] != 'L') ? ko->name : ko->topic.name;		// all LEADER consumer groups have the same name. Differenciate them by topic name
	size_t len = 0;
	BUG_ON((timeout_ms != 0) || (!ko->topic.is_active));
	m->err = RD_KAFKA_RESP_ERR_NO_ERROR;

	/* Delegate message selection to the management simulator */
	m->payload = mgmt_sim_next_kafka_payload(unique_name, (int)ko->topic.cur_offset, &len);
	m->len = len;

	if (m->payload == NULL) {			// No message prepared to current consumer
		free(m);
		return NULL;
	}
	m->offset = ko->topic.cur_offset++;
	N_Tf(__AUTOID__, "consumer[@STR] ++cur_offset=@LD", unique_name, ko->topic.cur_offset);
	m->_private = NULL;
	return m;
}
