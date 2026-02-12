/*
 * sandbox_kafka_public.h - Kafka simulator public API for sandbox builds
 *
 * This header replaces <rdkafka.h> in the sandbox build.
 * It is included into the Toma build by toma/unitest/toma_in_sandbox.h.
 *
 * It provides the type definitions, constants, and function declarations
 * that production Toma code uses to interact with Kafka.
 *
 * Implemented according to: https://docs.confluent.io/platform/current/clients/librdkafka/html/rdkafka_8h.html
 * And https://github.com/confluentinc/librdkafka/blob/v2.13.0/src/rdkafka.h
 */
#ifndef NVMEIBT_TOMA_MSG_Q_API_H
#define NVMEIBT_TOMA_MSG_Q_API_H	// Bypass contents of "interfaces/nvmeibt_msg_queue_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { RD_KAFKA_OFFSET_BEGINNING = -2, RD_KAFKA_OFFSET_END  = -1,  RD_KAFKA_OFFSET_STORED = -1000, RD_KAFKA_OFFSET_INVALID = -1001};

typedef struct rd_kafka_s rd_kafka_t;
typedef struct rd_kafka_topic_s rd_kafka_topic_t;
typedef struct rd_kafka_conf_s rd_kafka_conf_t;
typedef struct rd_kafka_topic_conf_s rd_kafka_topic_conf_t;
typedef struct rd_kafka_queue_s rd_kafka_queue_t;
typedef struct rd_kafka_op_s rd_kafka_event_t;
typedef struct rd_kafka_topic_result_s rd_kafka_topic_result_t;
typedef struct rd_kafka_consumer_group_metadata_s rd_kafka_consumer_group_metadata_t;
typedef struct rd_kafka_error_s rd_kafka_error_t;
typedef struct rd_kafka_headers_s rd_kafka_headers_t;
typedef struct rd_kafka_group_result_s rd_kafka_group_result_t;
typedef struct rd_kafka_acl_result_s rd_kafka_acl_result_t;
typedef struct rd_kafka_Uuid_s rd_kafka_Uuid_t;
typedef struct rd_kafka_topic_partition_result_s rd_kafka_topic_partition_result_t;
typedef enum {
	RD_KAFKA_RESP_ERR_NO_ERROR = 0, RD_KAFKA_RESP_ERR__SSL = -50, RD_KAFKA_RESP_ERR__AUTHENTICATION, RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM, RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE, RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED, RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED, RD_KAFKA_RESP_ERR__FATAL, RD_KAFKA_RESP_ERR__PARTITION_EOF,
	RD_KAFKA_RESP_ERR__RETRY = -153, RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS = -175, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS = -174, RD_KAFKA_RESP_ERR__TRANSPORT = -195, RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE = 8, RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE = 15, RD_KAFKA_RESP_ERR_NOT_COORDINATOR = 16,
	RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN = -187, RD_KAFKA_RESP_ERR__TIMED_OUT = -185, RD_KAFKA_RESP_ERR__WAIT_COORD = -180, RD_KAFKA_RESP_ERR__WAIT_CACHE = -164, RD_KAFKA_RESP_ERR__DESTROY = -197, RD_KAFKA_RESP_ERR__INTR = -163, RD_KAFKA_RESP_ERR__MSG_TIMED_OUT = -192,
	RD_KAFKA_PARTITION_UA = -1
} rd_kafka_resp_err_t;

typedef struct rd_kafka_topic_partition_s {
	rd_kafka_t* k;			// Daniel: In real kafka no such pointer, we keep it to verify that caller does not mistakenly assign partition to wrong consumer
	const char *topic;
	int32_t partition;
	rd_kafka_resp_err_t err;
	int64_t offset;
} rd_kafka_topic_partition_t;

typedef struct rd_kafka_topic_partition_list_s {
	rd_kafka_topic_partition_t elems[1];
	int cnt, size;
} rd_kafka_topic_partition_list_t;

rd_kafka_topic_partition_list_t* rd_kafka_topic_partition_list_new(int n);
rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_add(    rd_kafka_topic_partition_list_t *, const char* name, int32_t partition);
void rd_kafka_topic_partition_list_destroy(rd_kafka_topic_partition_list_t*);

enum rd_kafka_type_t { RD_KAFKA_CONSUMER = 'C', RD_KAFKA_PRODUCER = 'P' };
char* rd_kafka_err2str(rd_kafka_resp_err_t e);
char* rd_kafka_err2name(rd_kafka_resp_err_t e);
rd_kafka_resp_err_t rd_kafka_last_error(void);
typedef enum { RD_KAFKA_CONF_UNKNOWN = -2,  RD_KAFKA_CONF_INVALID = -1001,  RD_KAFKA_CONF_OK = 0 } rd_kafka_conf_res_t;
typedef struct { char* payload; void *_private; int len; int offset; rd_kafka_resp_err_t err; } rd_kafka_message_t;
void rd_kafka_message_destroy(rd_kafka_message_t*msg);

rd_kafka_conf_t* rd_kafka_conf_new(void);
void rd_kafka_conf_destroy(rd_kafka_conf_t* me);
rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t*kc, const char *key, const char *val, char* err_str, size_t size_of_err);
rd_kafka_t* rd_kafka_new(enum rd_kafka_type_t who, rd_kafka_conf_t*cfg, char*err_str, size_t size_of_err);
void rd_kafka_conf_set_error_cb(    rd_kafka_conf_t*, void (*fn)(rd_kafka_t *rk, int err, const char *reason, void *opaque));
void rd_kafka_conf_set_dr_msg_cb(   rd_kafka_conf_t*, void (*fn)(rd_kafka_t *rk, const rd_kafka_message_t *kmsg, void *opaque));
void rd_kafka_conf_set_rebalance_cb(rd_kafka_conf_t*, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque));
void rd_kafka_conf_set_offset_commit_cb(rd_kafka_conf_t*, void (*fn)(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, void *opaque));
rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t* me);
const char*         rd_kafka_name(   const rd_kafka_t* me);
void                rd_kafka_set_log_level(rd_kafka_t* me, int lvl);
void                rd_kafka_flush(        rd_kafka_t* me, int x);
void                rd_kafka_destroy(      rd_kafka_t* me);
int                 rd_kafka_poll(         rd_kafka_t* me, bool is_blocking);
rd_kafka_resp_err_t rd_kafka_commit(       rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int is_async);
rd_kafka_resp_err_t rd_kafka_committed(    rd_kafka_t* me, rd_kafka_topic_partition_list_t* pl, int x);
rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *me, const char *str, int32_t partition, int64_t *low_oldest_beginning_offset, int64_t *high_newest_end_offset, int timeout);
rd_kafka_resp_err_t rd_kafka_position	(  rd_kafka_t *me, rd_kafka_topic_partition_list_t *pl);
rd_kafka_topic_conf_t* rd_kafka_topic_conf_new(void);
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *conf);
rd_kafka_topic_t* rd_kafka_topic_new(rd_kafka_t *rk, const char* name, rd_kafka_topic_conf_t* conf);

void                rd_kafka_topic_destroy(rd_kafka_topic_t *kt);
rd_kafka_message_t* rd_kafka_consumer_poll(rd_kafka_t *rk, int timeout_ms);
rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk);
rd_kafka_resp_err_t rd_kafka_assign(     rd_kafka_t *, const rd_kafka_topic_partition_list_t *pl);
rd_kafka_resp_err_t rd_kafka_assignment (rd_kafka_t *,       rd_kafka_topic_partition_list_t **pl);
const char*         rd_kafka_topic_name(const rd_kafka_topic_t*);

static inline int rd_kafka_wait_destroyed(int n_msec) { (void)n_msec; return 0; }
enum my_rd_kafka_purge_flags { RD_KAFKA_PURGE_F_INFLIGHT = 0x2, RD_KAFKA_PURGE_F_NON_BLOCKING = 0x4 };
rd_kafka_resp_err_t rd_kafka_purge(rd_kafka_t * rk, int purge_flags);
enum my_rd_kafka_producer_flags { RD_KAFKA_MSG_F_FREE = 0x1, RD_KAFKA_MSG_F_COPY = 0x2 };
int rd_kafka_produce(rd_kafka_topic_t *kt, int32_t partition, int msgflags, void *payload, size_t len, const void *key, size_t keylen, void *msg_opaque);
rd_kafka_resp_err_t rd_kafka_fatal_error(rd_kafka_t *rk, char *errstr, size_t errstr_size);
static inline int         rd_kafka_version(    void)	{ return 0x20102; }
static inline const char* rd_kafka_version_str(void)	{ return "0x20102"; }

#endif // NVMEIBT_TOMA_MSG_Q_API_H
