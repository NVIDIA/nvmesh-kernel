/*
 * mgmt_sim.h - Management simulator for Toma sandbox unit tests
 *
 * This module simulates the management server's Kafka message exchanges with Toma.
 * It provides message templates and tracks state for test scenarios.
 */
#ifndef TOMA_UNITEST_MGMT_SIM_H
#define TOMA_UNITEST_MGMT_SIM_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Initialize the management simulator.
 * @param my_hostname The hostname of the simulated Toma node.
 */
void mgmt_sim_init(const char *my_hostname);

/**
 * Get the next Kafka message payload to deliver to Toma.
 * The returned buffer is owned by the caller and must be freed.
 *
 * @param consumer_name The Kafka consumer name (e.g., "HW", "CMD", topic name).
 * @param out_len       Output: length of the returned payload (0 if no message).
 * @return Allocated payload buffer, or NULL if no message is available.
 */
char *mgmt_sim_next_kafka_payload(const char *consumer_name, size_t *out_len);

/**
 * Called when Toma produces a message (e.g., reportTarget, keepalive).
 * The simulator can inspect and track these messages.
 *
 * @param payload The message payload (not necessarily NUL-terminated).
 * @param len     Length of the payload.
 */
void mgmt_sim_on_toma_produced(const void *payload, size_t len);

/**
 * Get the last captured reportTarget JSON (for test assertions).
 * @return The last reportTarget JSON string, or NULL if none received.
 *         The returned pointer is owned by the simulator; do not free.
 */
const char *mgmt_sim_get_last_report_target(void);

/**
 * Destroy the management simulator and free resources.
 */
void mgmt_sim_destroy(void);

#endif /* TOMA_UNITEST_MGMT_SIM_H */
