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

struct kafka_simulator_t;

struct kafka_simulator_t *sandbox_kafka_init(void);
void sandbox_kafka_destroy(struct kafka_simulator_t *ks);

#endif // TOMA_SANDBOX_KAFKA_INTERNAL_H
