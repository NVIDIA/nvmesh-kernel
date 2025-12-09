#ifndef NVMEIBT_KAFKA
#define NVMEIBT_KAFKA

#include "nvmeibt_common.h"
#include "../common/nvmeib_shared.h"
#include "nvmeibt_params.h"
#include "nvmeibt_uuid.h"
#include "nvmeibt_ds.h"
#include "interfaces/nvmeibt_msg_queue_api.h"

/* kafka_offset life-cycle
 * There are several variables that hold the kafka_offset :
 *  k_incremental_updates_consumer_offset	// Only the leader actually uses it
 *  										// A target-node that was just added (has no persistence) also reads it, and can become a raft candidate only if it is the first added target
 *  										// When kafka reads a new record it sends it to the toma leader using a TOMA_WAKEUP_TYPE_KAFKA
 *
 *  leader_kafka_offset_mgmt;					// Updated upon TOMA_WAKEUP_TYPE_KAFKA, when updating the mgmt_config
 *	leader_kafka_offset_calculating;			// The offset_mgmt used in calculate, will become offset_to_commit upon successful calculate
 *	leader_kafka_offset_to_commit;				// Updated after calc (that updated from mgmt config)
 *	leader_kafka_offset_committed_by_majority;	// Updated when offset has a majority
 *	follower_kafka_offset_submitted;			// To persistence
 *	follower_kafka_offset_committed;			// On persistence
 *  follower_kafka_offset_applied;				// When told to apply
 *
 * 	leader_kafka_offset_blocking_incremental_TARGET_updates	// Can continue when == (KAFKA_OFFSET, leader_committed_by_majority)
 */

#define KAFKA_TOPIC_CHANGE_NO 1		// Between 3.2 and 3.3 we changed the kafka topics naming convention (change_no went 0-->1)

#define MGMT_LOG_MSG_HEADER_LEN		96
#define MGMT_LOG_MSG_MSG_LEN		256

#define NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN (HOST_NAME_MAX + 30)
#define KAFKA_PRODUCER_MSG_HEADER_FMT   "\"originType\": \"TOMA\", \"messageType\": \"%s\", \"messageTypeVersion\": %u, \"hostname\": \"%s\", \"tomaToken\": %lld, \"messageSequence\": %lu,  \"leaderToken\": null, "
#define KAFKA_PRODUCER_MSG_HEADER_FMT_L "\"originType\": \"TOMA\", \"messageType\": \"%s\", \"messageTypeVersion\": %u, \"hostname\": \"%s\", \"tomaToken\": %lld, \"messageSequence\": %lu,  \"leaderToken\": %lld, "
#define KAFKA_PRODUCER_MSG_HEADER_VAR(  mType, ver) mType, ver, nvmeibt_get_my_hostname(), nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt(), get_next_running_producer_msg_sequence_number()
#define KAFKA_PRODUCER_MSG_HEADER_VAR_L(mType, ver) mType, ver, nvmeibt_get_my_hostname(), nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt(), get_next_running_producer_msg_sequence_number(), nvmeibt_kafka_get_leader_keepalive_token_provided_by_mgmt()

union offset_with_topic_change_no {
	int8_t		a[8];	// Naturally a[0] is the LSB and a[7] is the MSB
	int64_t		all;
};

int64_t purify_offset(int64_t offset_with_topic_change_no);
int8_t get_topic_change_no_from_offset(int64_t offset_with_topic_change_no);
#define nvmeibt_offset_and_idx_uninitialized (-1LL)
static inline bool nvmeibt_offset_and_idx_is_uninitialized(int64_t offset_and_idx)
{
	return (purify_offset(offset_and_idx) == nvmeibt_offset_and_idx_uninitialized);
}

extern int64_t volatile 			kafka_leader_offset_blocking_incremental_TARGET_updates;
#define NVMEIBT_KAFKA_SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(name, _new_offset_)	({																				\
	int64_t				_offset_ = _new_offset_;																																		\
	N_Tf(name ## 1, "SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(@INT64_TX-->@INT64_TX)", kafka_leader_offset_blocking_incremental_TARGET_updates, _offset_);			\
	NTOMA_ASSERT(name ## 2, nvmeibt_offset_and_idx_is_uninitialized(_offset_) ||																										\
							nvmeibt_offset_and_idx_is_uninitialized(kafka_leader_offset_blocking_incremental_TARGET_updates) ||															\
							(purify_offset(_offset_) > purify_offset(kafka_leader_offset_blocking_incremental_TARGET_updates)),															\
				 "OOPS SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(@INT64_TX-->@INT64_TX)", kafka_leader_offset_blocking_incremental_TARGET_updates, _offset_);			\
	kafka_leader_offset_blocking_incremental_TARGET_updates = _offset_;																													\
})

enum KAFKA_EVENT_TYPE {
	KAFKA_EVENT_TYPE_UNKNOWN						= 0,
	KAFKA_EVENT_TYPE_VOL_ADD						= 1,
	KAFKA_EVENT_TYPE_VOL_DEL						= 2,
	KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED				= 3,
	KAFKA_EVENT_TYPE_VOL_UPD						= 4,
	KAFKA_EVENT_TYPE_TARGET_ADD						= 6,
	KAFKA_EVENT_TYPE_TARGET_DEL						= 7,
	KAFKA_EVENT_TYPE_HW_FULL_CONFIG					= 9,
	KAFKA_EVENT_TYPE_CMD							= 10,
};

enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY {
	NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_UNKNOWN		= 0,
	NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH			= 1,
	NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW			= 2,
};

enum ENCRYPT_CMD_RESPONSE {
	ENCRYPT_CMD_RESPONSE_SUCCESS				= 1,
	ENCRYPT_CMD_RESPONSE_CMD_ERR				= 2,
	ENCRYPT_CMD_RESPONSE_TOMA_ERR				= 3,
	ENCRYPT_CMD_RESPONSE_UNSEEN					= 4,
	ENCRYPT_CMD_RESPONSE_MANUAL_ACTION_NEEDED	= 5
};

struct messageType_params_ctx {
	char		messageType[64];
	size_t		messageType_len;
	int64_t 	messageTypeVersion;
	int64_t		version;		// The ordering between msgs. It might be that the mgmt producer will not push them to kafka ordered, so TOMA needs to use the version
};

struct name_and_uuid_params_ctx {
	char				hostname[NVMEIB_HOST_NAME_LEN];
	union nvmeib_uuid	uuid;
	int					n_members_total_before_add_del;
	int64_t				targets_updates_sequence;
};

struct kafka_wakeup_params {
	enum KAFKA_EVENT_TYPE				event_type;
	struct messageType_params_ctx		messageType_params;
	int64_t								kafka_offset;
	unsigned long long					kafka_raft_term_when_started_consuming_leader_msgs;
	void								*event_data;
	// The following is for the sorted queue of incoming msgs
	int64_t								seq_no;
	struct xdlist 						kafka_raft_members_sorted_msgs_queue_link;
};


/******* Declarations of the ".c" functions ********/

int64_t nvmeibt_kafka_get_kafka_mgmt_zone_number(void);
bool nvmeibt_kafka_is_mgmt_zone_specified(void);
uint64_t get_next_running_producer_msg_sequence_number(void);
int64_t nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt(void);
int64_t nvmeibt_kafka_get_leader_keepalive_token_provided_by_mgmt(void);
int nvmeibt_kafka_generic_log_msg_to_mgmt_send(const char *unique_key, char *header, char *str, enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY priority);
void nvmeibt_kafka_new_kafka_mgmt_zone_number_received (int64_t zone_number);

void nvmeibt_kafka_set_last_sent_to_toma_targets_updates_seq_no(int64_t seq_no);

// int nvmeibt_kafka_init(void);
void nvmeibt_kafka_upd_from_nvmesh_conf(void);
int nvmeibt_kafka_launch(void);
void nvmeibt_kafka_shutdown(void);
bool nvmeibt_kafka_is_kafka_done_shutdown(void);

void nvmeibt_kafka_req_stop_consuming_leader_VOL_msgs(void);
void nvmeibt_kafka_req_stop_consuming_leader_TARGET_msgs(void);
void nvmeibt_kafka_req_start_consuming_leader_VOL_msgs(int64_t kafka_offset_VOL, unsigned long long raft_term);
void nvmeibt_kafka_req_start_consuming_leader_TARGET_msgs(int64_t kafka_offset_TARGET, int64_t seq_no_TARGET, unsigned long long raft_term);

void nvmeibt_kafka_outgoing_msgs_queue_add(const char *unique_key, const char *val, size_t val_len, enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY outgoing_msg_priority);

void nvmeibt_kafka_send_encrypt_cmd_response(const char *vol_name,
											 const struct nvmeibt_urn_uuid *vol_uuid,
											 int encrypt_idx,
											 enum ENCRYPT_CMD_RESPONSE error_code,
											 bool is_retryable,
											 const char *error_str);
void nvmeibt_kafka_mark_CMD_k_msg_for_kafka_commit_by_toma(int64_t kafka_offset);

int nvmeibt_raft_print_kafka_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

/******* Static inline forward declarations ********/
/******* Static inline with no external dependencies ********/
/******* Includes needed for static inline functions ********/
/******* Static inline functions that depend on other functions ******/

#endif	// #ifdef NVMEIBT_KAFKA
