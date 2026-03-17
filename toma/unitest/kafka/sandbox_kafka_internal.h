/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef TOMA_SANDBOX_KAFKA_INTERNAL_H
#define TOMA_SANDBOX_KAFKA_INTERNAL_H
/* Provides interface to the Kafka simulator and broker backend. Used only by simulators/unit-tests. */
#include "../sandbox_util.h"

enum sim_topic_type_toma_to_mgmt {	// Supported Kafka topics for Toma<-->Mgmt communication
	KTOPIC_TYPE_T2M_UNKNOWN = '?', KTOPIC_TYPE_T2M_PRIORITY = 'P', KTOPIC_TYPE_T2M_KEEPALIVE = 'K', KTOPIC_TYPE_T2M_LOW = 'L',
	KTOPIC_TYPE_M2T_HW_CFG = 'H', KTOPIC_TYPE_M2T_CMD = 'C', KTOPIC_TYPE_M2T_TARGETS_RAFT = 'R', KTOPIC_TYPE_M2T_VOLUMES = 'V',
};

struct sim_broker_topic;
struct rd_kafka_message_s;
struct sim_broker_topic *sim_broker_topic_find_by(enum sim_topic_type_toma_to_mgmt type);
void sim_broker_topic_msg_produce(struct sim_broker_topic *t, void *payload, size_t len, const bool should_copy);
bool sim_broker_topic_msg_consume(struct sim_broker_topic *t, struct rd_kafka_message_s *rv);	// Returns true if msg was consumed and fills rv.
void sim_broker_topic_ack_offsets(struct sim_broker_topic *t, int64_t ack_offset);		// Ack that consumer is done with this offset and all which are smaller

struct kafka_simulator_t;
struct kafka_simulator_t *sandbox_kafka_init(void (*notify_mgmt_simu_toma_send_msg)(struct sim_broker_topic *t));
void sandbox_kafka_destroy(struct kafka_simulator_t *ks);

#endif // TOMA_SANDBOX_KAFKA_INTERNAL_H
