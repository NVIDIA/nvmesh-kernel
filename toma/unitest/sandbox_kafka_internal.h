/*
 * sandbox_kafka_internal.h - Kafka simulator internal interface for sandbox code
 *
 * Provides the init/destroy interface for the Kafka simulator state.
 * The kafka_simulator_t struct is opaque here; its definition lives in sandbox_kafka.c.
 * Other sandbox code (toma_in_sandbox.c) stores a pointer to it in the TSB struct
 * so that the full sandbox state is visible in a debugger.
 */
#ifndef TOMA_SANDBOX_KAFKA_INTERNAL_H
#define TOMA_SANDBOX_KAFKA_INTERNAL_H

#include "sandbox_util.h"

struct kafka_simulator_t;
struct kafka_simulator_t *sandbox_kafka_init(void);
void sandbox_kafka_destroy(struct kafka_simulator_t *ks);

// Supported Kafka topics for Toma<-->Mgmt communication
enum sim_topic_type_toma_to_mgmt {
	KTOPIC_TYPE_T2M_UNKNOWN = '?', KTOPIC_TYPE_T2M_PRIORITY = 'P', KTOPIC_TYPE_T2M_KEEPALIVE = 'K', KTOPIC_TYPE_T2M_LOW = 'L',
	KTOPIC_TYPE_M2T_HW_CFG = 'H', KTOPIC_TYPE_M2T_CMD = 'C', KTOPIC_TYPE_M2T_TARGETS_RAFT = 'R', KTOPIC_TYPE_M2T_VOLUMES = 'V',
};

struct sim_broker_topic;
struct rd_kafka_message_s;
void sim_broker_topic_msg_produce(struct sim_broker_topic *t, void *payload, size_t len, const bool should_copy);
bool sim_broker_topic_msg_consume(struct sim_broker_topic *t, struct rd_kafka_message_s *rv);	// Returns true if msg was consumed and fills rv.
void sim_broker_topic_ack_offsets(struct sim_broker_topic *t, int64_t ack_offset);		// Ack that consumer is done with this offset and all which are smaller

#endif // TOMA_SANDBOX_KAFKA_INTERNAL_H
