#ifndef NVMEIBM_MGMT_SIMU_H
#define NVMEIBM_MGMT_SIMU_H
/* Implementation of management simulator */
#include "./nvmeib_common_all.h"						// Must be first include
#include "nvmeibc_mcs_stub.h"							// Format of mgmt<-->clnt messages
// Todo: management should create CSV which will be converted by toma to the below internal struct. To save time daniel made this struct public and management service already creates topology for toma.

/******************************************************************************/
struct mgmt_alerts_log_simu { 		// Simulates logging of clients allerts to management (you can see them in the UI of mgmt under 'log' tab
	char *expected_header;			// Basic implementation: Stores 1 last log entry only
	char *expected_message;
	char *expected_level;
	int  msg_counter;
	bool expector_set;
};
void mgmt_alerts_log_simu_insert(    struct mgmt_alerts_log_simu* s, char *level, char *header, char *message);
void mgmt_alerts_log_simu_verify_msg(struct mgmt_alerts_log_simu* s, struct nvmeib_mcs_log *msg);

/******************************************************************************/
struct mcs_message_queue {		// Message queue of mgmt-->clnt messages. Used to store a single (or more later) MCS message And allow sending it at a later time
	const struct c_api_proc *mcs;	// Link to the /proc/nvmeibc/mcs/mcs of client to send msgs to client
	void *message;				// Stores a single message, Todo make a real queue
	unsigned len;				// Length of the stored message
	spinlock_t guard;
	int should_store;			// 0 - don't store, 1 - store, 3 - store and allow overriding previous messages (loosing messages)
};
void mcs_message_queue_init(        struct mcs_message_queue* mq);	// Initialized in init pre config
void mcs_message_queue_destroy(     struct mcs_message_queue* mq);	// Used to clear the structure
void mcs_message_queue_activate(    struct mcs_message_queue* mq, bool allow_override); 	// Tell mcs simulator to start enqueing incomming MCS message instead of forwarding them
bool mcs_message_queue_try_insert(  struct mcs_message_queue *mq, char *buf, unsigned len);
int  mcs_message_queue_flush_n_stop(struct mcs_message_queue* mq);	// flush all enqueued messages and stop the queue

/******************************************************************************/
typedef void (*mgmt_cb_func)(void* ctx);

//MCS simulator
struct mcs_simu {
	int inst_id;                   /* Client id */
	struct mgmt_alerts_log_simu s; /* Log of allerts from client */
	struct mcs_message_queue mq;   /* Message queue of to-be-sent msgs to client */

	//Set these using mcs_set_expected_counters() BEFORE sending a message to the client using generate_mcs_update_token()
	long long expected_reportID;     /* Expected report ID for each client for upstream messages verification */
	long long expected_sequence_num; /* Expected sequence number for each client for upstream messages verification */
	long long expected_client_token; /* Expected token for each client for upstream messages verification */
	unsigned long expected_keepalive_interval; /* The time between client --> mgmt KA messages */

	mgmt_cb_func on_ka_cb;
	void* ka_cb_ctx;

	u32 expected_messageTypeVersion;
};

#include "nvmeibm_conf_db.h"						// Needed NVMESH_N_MAX_CLIENTS
struct mgmt_simu {
	struct mcs_simu mcs[NVMESH_N_MAX_CLIENTS];		// Mcs simulator, one per client instance
	const struct mongo_db_simu *mdb;				// Pointer to mongo db with configuration. The db resites outside of mgmt
};

#define mcs_simu_get_mgmt(mcs) container_of((void *)((mcs) - (mcs)->inst_id), struct mgmt_simu, mcs)
#define mcs_simu_get_mdb(mcs) mcs_simu_get_mgmt(mcs)->mdb

void mgmt_simu_init(             struct mgmt_simu* mgmt, const struct mongo_db_simu *mdb);
void mgmt_simu_destroy(          struct mgmt_simu* mgmt);

void mgmt_simu_expect_alert(     struct mcs_simu* mcs, char *level, char *header, char *message); // Tell Mgmt that allert from client will arrive so it can verify the correctness of the allert
void mgmt_simu_verify_num_alerts(struct mcs_simu* mcs, int i);
void mgmt_init_expected_counters(struct mcs_simu* mgmt);

void mgmt_set_ka_callback(struct mcs_simu* mgmt, mgmt_cb_func on_ka_cb, void* ctx);
void mgmt_clear_ka_callback(struct mcs_simu* mgmt);

#endif // NVMEIBM_MGMT_SIMU_H
