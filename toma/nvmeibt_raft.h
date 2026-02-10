#ifndef NVMEIBT_RAFT
#define NVMEIBT_RAFT

//#include <time.h>
#include <asm/param.h>
#include "nvmeibt_common.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_persistency_info.h"

struct nvmeibt_node;
struct name_and_uuid_params_ctx;

/*******************       raft_commit_lifecycle_ctx       ********************/
#define     RAFT_COMMIT_LIFECYCLE_VAL(      _which_, _field_) (nvmeibt_raft_get_my_raft()->_which_##_commit_lifecycle._field_)
#define SET_RAFT_COMMIT_LIFECYCLE_VAL(name, _which_, _field_, new_idx) ({																			\
	struct raft_commit_lifecycle_ctx	*_lifecycle = &(nvmeibt_raft_get_my_raft()->_which_##_commit_lifecycle);									\
	const int64_t						_val = (new_idx);																							\
	if (	(offsetof(struct raft_commit_lifecycle_ctx, _field_) == offsetof(struct raft_commit_lifecycle_ctx, leader_to_commit)) ||				\
			(offsetof(struct raft_commit_lifecycle_ctx, _field_) == offsetof(struct raft_commit_lifecycle_ctx, leader_committed_by_majority))) {	\
		if ((_val < RAFT_COMMIT_LIFECYCLE_VAL(_which_, _field_) && _val != nvmeibt_offset_and_idx_uninitialized)) {										\
			N_Wf(name ## _error, "SET_RAFT_COMMIT_LIFECYCLE_commit_lifecycle_ctx_VAL(" MACRO_DEF_TO_STR(_which_) ", " MACRO_DEF_TO_STR(_field_) ")@INT64_TX>@INT64_TX",		\
				 RAFT_COMMIT_LIFECYCLE_VAL(_which_, _field_), _val);																				\
			nvmeibt_abort(ES_FATAL);																												\
		}																																			\
	}																																				\
	if (_lifecycle->_field_ != _val) {																												\
		_lifecycle->_field_ = _val;																													\
		N_Tf(name, "SET_RAFT_COMMIT_LIFECYCLE_VAL(" MACRO_DEF_TO_STR(_which_) ", " MACRO_DEF_TO_STR(_field_) ")=@INT64_TX", _lifecycle->_field_);	\
	}																																				\
})

struct raft_commit_lifecycle_ctx {
	int64_t								leader_calculated;				// Not really part of the commit life-cycle
	//
	int64_t								leader_to_commit;				// Updated after calc
	int64_t								leader_committed_by_majority;	// Updated when idx has a majority
	int64_t								follower_submitted;				// Submitted to persistence
	int64_t								follower_committed;
	int64_t								follower_applied;
	int64_t								follower_to_apply;
	int64_t								follower_sent_to_leader;		// Currently used only for raft_members_idx (kafka_offset)
	//
	int 								leader_n_peers_committed;
};

/*******************       -------------------------       ********************/

#include "nvmeibt_wq.h"

enum RAFT_ROLE_TYPE {
	RAFT_ROLE_UNKNOWN = (0x0),
	RAFT_ROLE_LEADER = (0x1 << 0),
	RAFT_ROLE_CANDIDATE = (0x1 << 1),
	RAFT_ROLE_FOLLOWER = (0x1 << 2),
};

struct nvmeibt_raft_member {
	char									hostname[NVMEIB_HOST_NAME_LEN];
	union nvmeib_uuid						uuid;
	struct nvmeibt_urn_uuid					urn_uuid;
	struct nvmeibt_node						*its_node;
	struct nvmeibt_disk						*disks_leader[NVMEIBT_MAX_N_DISKS];
	int64_t									kafka_offset;
	int										n_disks_leader;
	struct mm_raft_member_conf				this_member_leader_serialized_wire_buf;
	unsigned int							toma_software_version;
	bool									is_me;
	//
	struct timespec							last_received_voted_for_me_timespec;
	struct nvmeibt_persist_and_wire_buf		committed_persist_and_wire_buf_hdr;			// Holds just the "header", and not the data (topo, config, ...)
	unsigned long long						last_local_serialization_version;
	uint32_t								last_append_entries_rep_msg_num;
	BOOL									is_vote_valid;
	BOOL									is_alive_for_topo;
	struct timespec							is_alive_for_topo_start_timespec;
	BOOL									is_ignored;
	int										config_tag;
	//
//	struct xdlist							members_link;
};

static inline char *nvmeibt_raft_member_name(struct nvmeibt_raft_member *member)
{
	return (member ? member->hostname : "");
}

static inline union nvmeib_uuid* nvmeibt_raft_member_id(struct nvmeibt_raft_member *member)
{
	return (member ? &(member->uuid) : NULL);
}

static inline char *nvmeibt_raft_member_id_str(struct nvmeibt_raft_member *member)
{
	return (member ? member->urn_uuid.str : "");
}

static inline struct nvmeibt_node *nvmeibt_raft_member_get_node(struct nvmeibt_raft_member *member)
{
	return (member ? member->its_node : NULL);
}

struct nvmeibt_raft_ctx {
	// Auxiliary data
	BOOL					is_leader_mature;
	BOOL					is_leader_ever_committed_by_majority;
	BOOL					is_follower_report_to_mgmt_due;
	unsigned long long		shutdown_term;
	unsigned long long		leader_shutdown_term;
	unsigned long long		first_append_entries_term;
	struct nvmeibt_raft_member	*my_member;
	// Raft state
	union nvmeib_uuid		leader_uuid;
	char					leader_node_name[NVMEIB_HOST_NAME_LEN];
	enum RAFT_ROLE_TYPE		role;
	// voted_for_me is used by candidate. Abused by leader (to count current followers)
	int						n_peers_voted_for_me;
	struct timespec			last_time_leader_had_a_majority;
	int						leader_last_2_3rds_majority_timestamp_sec;
	//
	struct raft_commit_lifecycle_ctx	TOPO_commit_lifecycle;
	struct raft_commit_lifecycle_ctx	TOPO_CONFIG_commit_lifecycle;
	struct raft_commit_lifecycle_ctx	KAFKA_MGMT_CONFIG_commit_lifecycle;
	struct raft_commit_lifecycle_ctx	RAFT_MEMBERS_commit_lifecycle;
	struct raft_commit_lifecycle_ctx	RAFT_MEMBERS_SEQ_NO_commit_lifecycle;
	struct raft_commit_lifecycle_ctx	current_raft_TERM_commit_lifecycle;
	// We have 4 sections in the persist_and_wire_buf:
	// - TOPO, TOPO_CONFIG, KAFKA_MGMT_CONFIG, RAFT_MEMBERS
	// The leader serializes a buffer per section on every we_have_a_new_baseline
	struct nvmeibt_Buf			leader_to_commit_wire_topo;
	struct nvmeibt_Buf			leader_to_commit_wire_topo_config;
	struct nvmeibt_Buf			leader_to_commit_wire_kafka_mgmt_config;
	struct nvmeibt_Buf			leader_to_commit_wire_raft_members;
	int64_t						applied_raft_members_seq_no;
	// When the leader sends an APPEND_ENTRIES it first generates (roughly speaking a concatenation of the above):
	// - leader_to_commit_persist_and_wire_buf_full
	// - leader_to_commit_persist_and_wire_buf_topo_only
	// The follower receives a persist_and_wire_buf and updates its follower_to_commit_persist_and_wire_buf_full
	// - Next it is submitted, and updates the committed lot(s)
	//   - The committed lots are updated prematurely, since they are used only later on by:
	//     - apply
	//     - convert_to_leader (candidate)
	// - Once committed, it is reported "committed" to the leader
	// - (A follower can use the "to_commit" content when switching to leader, since as a leader it first needs the majority to commit to this "to_commit", just like the original leader)
	// The members_list is used only by the leader.
	// - The leader updates it immediately when it gets an incremental-update
	// - A candidate starts from the to_commit RAFT_MEMBERS buffer (and offset), even if not yet committed (or even submitted)
	//
	// Serialization overview:
	// - config:
	//   - JSON --> conf --> parse(add_seg etc.)
	//   - seg --> ?conf? --> packed --> wire
	//   - wire --> packed --> conf --> parse
	// - topo
	//   - praid --> packed --> wire
	//   - wire --> packed --> parse(upd_praid etc.)
	TODO(Replace follower_to_commit_persist_and_wire_buf_full that is used as to_submit & submitted & committed by 3 different buffers, so that we can apply a committed buf although we have a new submitted (different praids topos changed));
	struct nvmeibt_persist_and_wire_buf		*follower_to_commit_persist_and_wire_buf_full;
	struct nvmeibt_persist_and_wire_buf		*leader_to_commit_persist_and_wire_buf_full;
	struct nvmeibt_persist_and_wire_buf		*leader_to_commit_persist_and_wire_buf_topo_only;
	struct nvmeibt_persist_and_wire_buf		*follower_to_leader_wire_buf;
	//
	// committed == (raft's)matched
	unsigned long long		leader_committed_LOG_index;		// Updated when (any) leader knows this index has a majority, propagated
	// Persistent state on all servers
	unsigned long long		current_term;					// Latest I've seen
	unsigned long long		last_rx_append_entries_term;
	uint32_t				last_rx_append_entries_msg_num;
	uint32_t				last_tx_append_entries_msg_num;
	union nvmeib_uuid		voted_for_uuid;
	// Timeout
	struct timespec			next_election_time;
	struct timespec			next_leader_heartbeat_timespec;
	int64_t					last_recieved_append_entries_timestamp_sec;	// Initialized to TOMA "boot" time
	struct timespec			time_converted_to_leader;
	struct nvmeib_hash_table	*raft_members_hash_by_uuid;
	int						n_raft_members;
	int						n_raft_active_members;
	uint32_t				guaranteed_sw_ver;
//	struct nvmeibt_Buf		serialized_members;
	struct timespec			leader_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec;
};

struct nvmeibt_disk_segment;
struct nvmeibt_persistency_wq_entry;

int64_t nvmeibt_raft_get_praid_leader_max_nsec_wait_for_registrable_seg_to_apply(BOOL client_problems);
int64_t nvmeibt_raft_get_praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply(BOOL client_problems);
void nvmeibt_raft_set_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC(int64_t leader_heartbeat_timeout_usec);
void nvmeibt_raft_set_effective_min_election_timeout_factor_USED_ONLY_BY_RPC(int64_t min_election_timeout_factor);
void nvmeibt_raft_set_effective_leader_heartbeat_timeout_ns(int64_t leader_heartbeat_timeout_nsec, bool is_enforcing);
void nvmeibt_raft_set_effective_min_election_timeout_factor(int64_t min_election_timeout_factor, bool is_enforcing);
int64_t nvmeibt_raft_get_effective_heartbeat_timeout_ns(void);
int64_t nvmeibt_raft_get_effective_min_election_timeout_factor(void);
int64_t nvmeibt_raft_get_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC(void);
void nvmeibt_raft_set_max_wait_for_non_registrable_seg_sec(int64_t wait_sec);
int64_t nvmeibt_raft_get_max_wait_for_non_registrable_seg_sec(void);

extern struct nvmeibt_raft_ctx     my_raft_global;
static inline struct nvmeibt_raft_ctx *nvmeibt_raft_get_my_raft(void)
{
	return &my_raft_global;
}

static inline unsigned long long nvmeibt_raft_get_current_term(void)
{
	return (nvmeibt_raft_get_my_raft()->current_term);
}

#define SET_RAFT_LEADER_NEXT_TOPOLOGY_VERSION(name) ({ \
	const int64_t val = ((int64_t)nvmeibt_raft_get_current_term() << 32) | (((RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit) + 1) & 0xffffffff)); \
	SET_RAFT_COMMIT_LIFECYCLE_VAL(name, TOPO, leader_calculated, val); \
})

static inline int nvmeibt_raft_get_leader_last_2_3rds_majority_timestamp_sec(void)
{
	return nvmeibt_raft_get_my_raft()->leader_last_2_3rds_majority_timestamp_sec;
}

static inline char *nvmeibt_leader_host_name(void)
{
	return nvmeibt_raft_get_my_raft()->leader_node_name;
}

static inline unsigned long long nvmeibt_raft_get_guaranteed_sw_ver(void)
{
	return (nvmeibt_raft_get_my_raft()->guaranteed_sw_ver);
}

static inline void nvmeibt_raft_set_guaranteed_sw_ver(uint32_t guaranteed_sw_ver)
{
	nvmeibt_raft_get_my_raft()->guaranteed_sw_ver = guaranteed_sw_ver;
}

static inline unsigned long long nvmeibt_raft_is_leader_ever_committed_by_majority(void)
{
	return (nvmeibt_raft_get_my_raft()->is_leader_ever_committed_by_majority);
}

const union nvmeib_uuid *nvmeibt_raft_get_voted_for_uuid(void);
void nvmeibt_raft_set_voted_for_uuid(const union nvmeib_uuid *voted_for_uuid);
BOOL nvmeibt_raft_is_raft_shutdownable_now(void);
void nvmeibt_raft_upd_shutdown_term(unsigned long long shutdown_term);
BOOL nvmeibt_raft_is_shutdown_triggered(void);
int nvmeibt_raft_save_toma_state_to_persistency(struct nvmeibt_persistency_wq_entry *entry);
int nvmeibt_raft_read_persistence_and_upd_committed(const char *toma_persistency_file_name, struct nvmeibt_Str *JSON_output);
BOOL nvmeibt_raft_is_same_msg(void *msg1, void *msg2);
int nvmeibt_raft_get_avg_leader_lifespan_sec(void);
void nvmeibt_raft_set_leader_uuid(const union nvmeib_uuid *leader_uuid);
BOOL nvmeibt_raft_leader_is_peer_vote_recent(struct nvmeibt_raft_member *member);
struct timespec nvmeibt_raft_get_next_timeout_timespec(void);
int nvmeibt_raft_timeout_occurred(BOOL is_sufficient_time_gap_before_raft, bool is_forced_reelect);
int nvmeibt_raft_handle_incoming_message(struct nvmeibt_big_msg *big_msg);
void nvmeibt_raft_activate(void);
// void nvmeibt_raft_node_was_removed(struct nvmeibt_node *node);
int nvmeibt_raft_one_time_init(void);
BOOL nvmeibt_raft_is_leader(void);
BOOL nvmeibt_raft_is_raft_valid(void);
void nvmeibt_raft_convert_to_leader(void);
int nvmeibt_raft_get_time_without_leader_sec(void);
int nvmeibt_raft_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
int nvmeibt_raft_print_status_json(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
int nvmeibt_leader_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
void nvmeibt_raft_fill_persistence_buf_for_system_disk(struct nvmeibt_Buf *buf_to_save);
void nvmeibt_raft_apply_committed_config_and_topo_as_needed(void);
void nvmeibt_raft_try_to_send_spurious_APPEND_ENTRIES_REP_or_REQ_VOTE_REP(struct nvmeibt_node *dst_node, bool is_req_vote);
void nvmeibt_raft_set_discard_append_entries(int discard_num, bool is_permanent);

int nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(const struct nvmeibt_persist_and_wire_buf *src, int8_t tlv_type, char **out_data);
int nvmeibt_raft_leader_copy_committed_persist_and_wire_buf_sections_into_separate_to_commit_bufs(void);
struct nvmeibt_persist_and_wire_buf *nvmeibt_raft_generate_persist_and_wire_buf(
	unsigned long long current_raft_term,
	unsigned long long last_rx_append_entries_raft_term,
	int64_t kafka_mgmt_zone_number,
	const union nvmeib_uuid *voted_for_raft_member_uuid,
	const union nvmeib_uuid *mgmt_DB_uuid,
	int64_t raft_calculated_append_entries_rep_time_ns,
	int64_t raft_calculated_topo_calc_time_ns,
	uint32_t guaranteed_sw_ver,
	//
	int64_t topo_idx, int64_t topo_seq_no, char *topo_data, int topo_data_len,
	int64_t topo_config_idx, int64_t topo_config_seq_no, char *topo_config_data, int topo_config_data_len,
	int64_t mgmt_config_offset, int64_t mgmt_config_seq_no, char *mgmt_config_data, int mgmt_config_data_len,
	int64_t members_offset, int64_t members_seq_no, char *members_data, int members_data_len);

struct nvmeibt_raft_member *nvmeibt_raft_get_member_by_id(const union nvmeib_uuid *id);
void nvmeibt_raft_link_member_to_node(struct nvmeibt_raft_member *member, struct nvmeibt_node *node, const union nvmeib_uuid *uuid);
void raft_leader_regenerate_the_two_to_commit_persist_and_wire_bufs_as_needed(void);
void nvmeibt_raft_leader_generate_leader_to_commit_wire_raft_members_buf(void);
void nvmeibt_raft_unlink_member_from_node(struct nvmeibt_raft_member *member, struct nvmeibt_node *node);
void nvmeibt_raft_add_member(char *hostname, int n_raft_members_total_before_add_del, const union nvmeib_uuid *uuid, bool is_incremental_add_fr_mgmt, int64_t kafka_offset, int config_tag);
void nvmeibt_raft_del_member(char *hostname, int n_raft_members_total_before_add_del, const union nvmeib_uuid *uuid, bool is_incremental_del_fr_mgmt, int64_t kafka_offset);
void nvmeibt_raft_del_all_members_at_exit(void);
int nvmeibt_raft_ignore_member(char *hostname);
void nvmeibt_raft_align_members_with_committed_wire_buf(struct nvmeibt_Str *JSON_output);
// calculated (adaptive)
int64_t nvmeibt_raft_leader_get_append_entries_rep_time_ns_for_persist_and_wire_buf(void);
int64_t nvmeibt_raft_leader_get_topo_calc_time_ns_for_persist_and_wire_buf(void);
void nvmeibt_raft_leader_set_calculated_append_entries_rep_time_ns_USED_ONLY_BY_JSON_PARSER(int64_t calculated_append_entries_rep_time_ns);
void nvmeibt_raft_leader_set_calculated_topo_calc_time_ns_USED_ONLY_BY_JSON_PARSER(int64_t calculated_topo_calc_time_ns);
//
int64_t nvmeibt_raft_get_persistent_leader_append_entries_time_ns(void);
int64_t nvmeibt_raft_get_persistent_leader_topo_calc_time_ns(void);
void nvmeibt_raft_follower_upd_effective_raft_heartbeat_timeout_and_factor(void);
void nvmeibt_raft_calc_timeouts_based_on_IIRs(void);
void nvmeibt_raft_reset_leader_calculated_IIRs(void);

#endif // #ifndef NVMEIBT_RAFT

