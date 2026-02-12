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

/************** Cluster config *******************************/
struct sb_cluster_conf {
	char my_hostname[64];		// Live Toma (non sandbox, real hostname)
	struct sb_node_conf {
		const char *hostname;	// Easily recognizable host name
		char uuid[40];			// Hex as string format (36 characters + NULL terminated string)
		uint64_t uuid16b[2];	// Packed 16[b] format
	} nodes[3], *live, *other;	// Cluster of 3 machines, 1 live followed by 2 simulated other tomas
	int n_nodes;
};
void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);

/*********************************************/

struct mgmt_sim_state;
struct mgmt_sim_state *mgmt_sim_init(struct sb_cluster_conf *initialized_cfg);

/* Get the next Kafka message payload to deliver to Toma. The returned buffer is owned by the caller and must be freed.*/
char *mgmt_sim_next_kafka_payload(const char *consumer_name, int queue_offset, size_t *out_len);

enum sim_topic_type_toma_to_mgmt {
	KTOPIC_TYPE_T2M_UNKNOWN = '?', KTOPIC_TYPE_T2M_PRIORITY = 'P', KTOPIC_TYPE_T2M_KEEPALIVE = 'K', KTOPIC_TYPE_T2M_LOW = 'L',
};

void mgmt_sim_on_toma_produced(enum sim_topic_type_toma_to_mgmt type, const void *payload, size_t len);

/**
 * Get the last captured reportTarget JSON (for test assertions).
 * @return The last reportTarget JSON string, or NULL if none received.
 *         The returned pointer is owned by the simulator; do not free.
 */
const char *mgmt_sim_get_last_report_target(void);

/**
 * Verify end-of-test conditions for the simulator.
 * Aborts the process if the format scenario did not complete successfully.
 */
void mgmt_sim_verify_at_end(void);

/**
 * Check if the format scenario state machine has completed.
 * @return true if the state machine reached the "done" state.
 */
bool mgmt_sim_is_done(void);

/**
 * Get the current state machine state name (for diagnostics).
 * @return Human-readable state name.
 */
const char *mgmt_sim_get_state_name(void);

/**
 * Destroy the management simulator and free resources.
 */
void mgmt_sim_destroy(void);

#endif /* TOMA_UNITEST_MGMT_SIM_H */
