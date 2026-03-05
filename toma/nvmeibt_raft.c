/*
 * A concensus algorithm (Raft) is needed in order to take consistent
 *  decisions. We need it
 *   - mostly in order to decide upon and synchronize the state of a RAID that
 *     can recover from a disk failure (RAID 1/5/6).
 *   - Any topology change, such as a disconnect of a group of nodes that hold
 *     a volume. The majority can decide to re-implement the volume on a different
 *     set of disks, and the old one should be invalidated.
 * The Only role of the Raft module is to constantly (or as needed), maintain
 *  a leader. Once a leader is chosen, it renewes its leadership periodically,
 *  and while doing so, it distributes topology changes.
 * A node that lost its leader (or never had one) is non-functinal
 * Based on
 *   In Search of an Understandable Consensus Algorithm (Extended Version)
 *   http://ramcloud.stanford.edu/raft.pdf
 *   page-4, Figure 2: "A condenced summary of the ..."
 *   https://ramcloud.stanford.edu/~ongaro/thesis.pdf
 *   Page 196 onwards
 *   An excellent introduction: http://thesecretlivesofdata.com/raft/
 * NOTE:
 *   Raft has an extensive LOG mechanism, whose role is to transfer and
 *   apply (ordered) the storage transactions on all the raft nodes
 *   We do not have such "incremental" scenario, and we only distribute
 *   the latest topology as-is when the leader calculates it.
 * TIMING
 *   - Any election invalidates the current leader (either when I convert to
 *     candidate, or upon reception of a valid req_vote).
 *   - The leader will declare itself invalid (to toma), if it doesn't see a
 *     majority for one RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC. It is checked
 *     before sending appernentries to all (due to heartbeat or new topology)
 *   - In case the leader is separated from the majority,
 *     RAFT_MAX_TIME_LEADER_SURVIVES_WITHOUT_MAJORITY_NSEC must expire before
 *     a new leader is chosen, elswhere.
 *
 * The Workflow is:
 * STATE = leader / follower / candidate
 * raft_timeout_window_start_time
 * Do forever {
 *   Wait for a message / (leader/follower/candidate)_timeout
 *   - If message (Raft / server / client)
 *     - process message
 *       - non_raft_messages are handled outside this file
 *     - adjust raft_timeout_window_start_time
 *   - else if timeout
 *     - Depending on state, take action
 *  }
 *
 *  Topology version
 *  - The topology_version is global, not per praid
 *  - There are two topology versions in the air
 *    - LAST_LOG - The most recent one that was accepted from the leader/persistency
 *      - TOMA is committed not to vote for any lower one in the future.
 *    - APPLIED_LOG - The one in use
 *      The leader will tell the TOMAs to use it after the leader verified that a majority
 *  	are committed to it LAST_LOG
 *  - It is a two phase commit. First distribute to a majority. Second apply.
 */
#include <sys/time.h>
#include <sys/stat.h>
#include <linux/unistd.h>
#include "nvmeibt_common.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_raft.h"
#include "interfaces/network/network_incs.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_node.h"
#include "nvmeibt_important_logs.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_disk_metadata.h"
#include "../common/nvmeib_shared.h"
#include "nvmeibt_persistency_info.h"
#include <signal.h>
#include "nvmeibt_kafka.h"
#include "nvmeibt_raft_msg_fmt.h"

#define RAFT_ROLE_STR() (									\
	my_raft_global.role == RAFT_ROLE_FOLLOWER ? "FOLLOWER" :		\
	my_raft_global.role == RAFT_ROLE_CANDIDATE ? "CANDIDATE" :	\
	my_raft_global.role == RAFT_ROLE_LEADER ? "LEADER" :			\
	"ROLE Unknown")

#define	FORCED_REELECT_RAFT_FLG											1
#define IS_FORCED_REELECT(msg) \
	(msg->flags & FORCED_REELECT_RAFT_FLG)

int		raft_time_from_startup_to_activation_sec = 10;

// Seems as if a single raft per cluster of nodes will do
struct nvmeibt_raft_ctx		my_raft_global;

static int dispatch_raft_msg(struct raft_msg *msg, struct nvmeibt_node *src_node);
static void convert_raft_msg_header_le_be(struct raft_msg *msg);

static void raft_leader_reset_counters_upon_last_LOG_change(void);

static int raft_num_of_messages_to_discard_after_change;
static int raft_num_of_messages_to_discard_after_change_tmp = 0;
static bool is_discard_after_change_permanent;

static inline const union nvmeib_uuid *raft_get_my_uuid(void)
{
	return nvmeibt_global_get_my_node_uuid();
}

static inline bool raft_is_my_uuid(const union nvmeib_uuid *uuid)
{
	const union nvmeib_uuid					*my_uuid = raft_get_my_uuid();

	return (my_uuid && ARE_UUID_EQ(uuid, my_uuid));
}

/******************************************************************************/

static inline const union nvmeib_uuid *raft_get_leader_node_uuid(void)
{
	return &(my_raft_global.leader_uuid);
}

static inline struct nvmeibt_node *raft_get_leader_node(void)
{
	return nvmeibt_node_get_node_by_id(raft_get_leader_node_uuid());
}

static inline bool raft_is_leader_uuid(const union nvmeib_uuid *uuid)
{
	return (uuid && ARE_UUID_EQ(raft_get_leader_node_uuid(), uuid));
}

static __attribute__((unused)) inline bool raft_am_I_leader(void)
{
	return (my_raft_global.my_member && ARE_UUID_EQ(raft_get_leader_node_uuid(), &(my_raft_global.my_member->uuid)));
}

static inline bool raft_do_we_have_a_leader(void)
{
	return !(ARE_UUID_EQ(raft_get_leader_node_uuid(), &nvmeib_uuid_null_val));
}

static inline bool raft_do_we_have_a_stable_leader(void)
{
	return (raft_do_we_have_a_leader() && (timespec_diff_ns(my_raft_global.next_election_time, nvmeibt_global_get_cur_event_start_time()) > nvmeibt_raft_get_effective_heartbeat_timeout_ns()));
}

static inline char *raft_get_leader_node_name(void)
{
	return nvmeibt_node_name(raft_get_leader_node());
}

/******************************************************************************/

const union nvmeib_uuid *nvmeibt_raft_get_voted_for_uuid(void)
{
	return (&(my_raft_global.voted_for_uuid));
}

void nvmeibt_raft_set_voted_for_uuid(const union nvmeib_uuid *voted_for_uuid)
{
	if (voted_for_uuid) {
		my_raft_global.voted_for_uuid = *voted_for_uuid;
	} else {
		my_raft_global.voted_for_uuid = nvmeib_uuid_null_val;
	}
}

static inline bool raft_is_voted_for_uuid(const union nvmeib_uuid *uuid)
{
	return (uuid && ARE_UUID_EQ(nvmeibt_raft_get_voted_for_uuid(), uuid));
}

static inline bool raft_do_we_have_a_voted_for(void)
{
	return !(ARE_UUID_EQ(nvmeibt_raft_get_voted_for_uuid(), &nvmeib_uuid_null_val));
}

/*********    TIMEOUTS   *************/

// RAFT related timeouts. Not fully specified in the spec
#define DEFAULT_MAX_ELECTION_TIMEOUT_FACTOR				(2)

#define CLIENT_PROBLEMS_REPORT_FACTOR					4
#define RAFT_TX_WAIT_FACTOR								2
//
#define CLIP_FLOOR_LEADER_HEARTBEAT_TIMEOUT_NS (RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT)
#define CLIP_ROOF_LEADER_HEARTBEAT_TIMEOUT_NS (5 * RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT)
#define CLIP_ROOF_IMAGINARY_REFERENCE_HEARTBEAT_TIMEOUT_NS (15 * RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT)
//
#define CLIP_FLOOR_LEADER_ELECTION_TIMEOUT_FACTOR (RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT)
#define CLIP_ROOF_LEADER_ELECTION_TIMEOUT_FACTOR (5 * RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT)
//
static bool						raft_leader_heartbeat_timeout___is_set_by_rpc = 0;
static bool						raft_min_election_timeout_factor___is_set_by_rpc = 0;
static int64_t					raft_leader_heartbeat_timeout_nsec = RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT;
static int64_t					raft_max_time_drifts_sum_nsec;
static int						raft_min_election_timeout_factor = RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT;
static int64_t					raft_min_election_timeout_nsec;
static int						raft_max_election_timeout_factor = DEFAULT_MAX_ELECTION_TIMEOUT_FACTOR;
static int64_t					raft_max_election_timeout_nsec;
static int64_t					raft_max_time_leader_survives_without_majority_nsec;
static int64_t					raft_max_time_non_responsive_member_is_considered_alive_for_topo_nsec;
static int64_t					praid_leader_max_nsec_wait_for_registrable_seg_to_apply;
static int64_t					praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply;
static bool						raft_is_incremental_wire_buf_enabled = false;

void nvmeibt_raft_set_incremental_wire_buf_enabled(bool is_enabled)
{
	if (raft_is_incremental_wire_buf_enabled != is_enabled) {
		N_Tf(jfdn1al, "raft_is_incremental_wire_buf_enabled: @BOOL-->@BOOL", raft_is_incremental_wire_buf_enabled, is_enabled);
		raft_is_incremental_wire_buf_enabled = is_enabled;
	}
}

static __kernel_suseconds_t		max_wait_for_non_registrable_seg_nsec = PRAID_LEADER_MAX_NSEC_WAIT_FOR_NON_REGISTRABLE_SEG_TO_APPLY_DEFAULT;

#define RAFT_RANDOM_ELECTION_TIMEOUT_NSEC	(raft_min_election_timeout_nsec + \
											(lrand48() % (raft_max_election_timeout_nsec - raft_min_election_timeout_nsec)))

#define LOG_LLD_VAR(var_name) N_Tf(var_name, #var_name "=@LLD", var_name)

void nvmeibt_raft_recalc_timeout_constants(void) {
	// RAFT related timeouts. Not fully specified in the spec
	// Mostly derived from raft_leader_heartbeat_timeout_nsec

	// Drifts: Heartbeat + 10x software clock resolution + 10 miliseconds
	LOG_LLD_VAR(raft_leader_heartbeat_timeout_nsec);
	raft_max_time_drifts_sum_nsec = (raft_leader_heartbeat_timeout_nsec / 10) + (SEC_TO_NSEC(1) / (HZ) * 2);
	LOG_LLD_VAR(raft_max_time_drifts_sum_nsec);
	raft_min_election_timeout_nsec = (raft_leader_heartbeat_timeout_nsec * raft_min_election_timeout_factor) + raft_max_time_drifts_sum_nsec;
	LOG_LLD_VAR(raft_min_election_timeout_nsec);
	raft_max_election_timeout_nsec = raft_min_election_timeout_nsec * raft_max_election_timeout_factor;
	LOG_LLD_VAR(raft_max_election_timeout_nsec);
	// The following are not mentioned anywhere in the RAFT spec. They are independent of each other
	raft_max_time_leader_survives_without_majority_nsec = raft_min_election_timeout_nsec;
	LOG_LLD_VAR(raft_max_time_leader_survives_without_majority_nsec);
	raft_max_time_non_responsive_member_is_considered_alive_for_topo_nsec = raft_min_election_timeout_nsec;			// 0.6s until node is declared dead, if no raft messages arrive
	LOG_LLD_VAR(raft_max_time_non_responsive_member_is_considered_alive_for_topo_nsec);
	praid_leader_max_nsec_wait_for_registrable_seg_to_apply = max(max_wait_for_non_registrable_seg_nsec * 2, raft_leader_heartbeat_timeout_nsec * 3);	// 10s until segments are declared dead, if not applied
	LOG_LLD_VAR(praid_leader_max_nsec_wait_for_registrable_seg_to_apply);
	praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply = max(max_wait_for_non_registrable_seg_nsec, raft_leader_heartbeat_timeout_nsec * 3);
	LOG_LLD_VAR(praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply);
}

int64_t nvmeibt_raft_get_effective_heartbeat_timeout_ns(void)
{
	return raft_leader_heartbeat_timeout_nsec;
}

int64_t nvmeibt_raft_get_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC(void)
{
	return (raft_leader_heartbeat_timeout_nsec / 1000);
}

int64_t nvmeibt_raft_get_effective_min_election_timeout_factor(void)
{
	return raft_min_election_timeout_factor;
}

void nvmeibt_raft_set_max_wait_for_non_registrable_seg_sec(int64_t wait_sec)
{
	max_wait_for_non_registrable_seg_nsec = SEC_TO_NSEC((uint64_t)wait_sec);
	nvmeibt_raft_recalc_timeout_constants();
}

int64_t nvmeibt_raft_get_max_wait_for_non_registrable_seg_sec(void)
{
	return (int64_t)(max_wait_for_non_registrable_seg_nsec / NSEC_IN_1_SEC);
}

int64_t nvmeibt_raft_get_praid_leader_max_nsec_wait_for_registrable_seg_to_apply(BOOL client_problems)
{
    if (client_problems)
		return praid_leader_max_nsec_wait_for_registrable_seg_to_apply / CLIENT_PROBLEMS_REPORT_FACTOR;
    else
		return praid_leader_max_nsec_wait_for_registrable_seg_to_apply;
}

int64_t nvmeibt_raft_get_praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply(BOOL client_problems)
{
    if (client_problems)
		return praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply / CLIENT_PROBLEMS_REPORT_FACTOR;
    else
		return praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply;
}

void nvmeibt_raft_set_effective_leader_heartbeat_timeout_ns(int64_t leader_heartbeat_timeout_nsec, bool is_enforcing)
{
	if (is_enforcing || !raft_leader_heartbeat_timeout___is_set_by_rpc) {
		if (leader_heartbeat_timeout_nsec && raft_leader_heartbeat_timeout_nsec != leader_heartbeat_timeout_nsec) {	// Do not overide with 0
			raft_leader_heartbeat_timeout_nsec = leader_heartbeat_timeout_nsec;
			N_Tf(7j03klt, "raft_leader_heartbeat_timeout_nsec=@LLD", raft_leader_heartbeat_timeout_nsec);
			nvmeibt_raft_recalc_timeout_constants();
		}
	}
}

void nvmeibt_raft_set_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC(int64_t leader_heartbeat_timeout_usec)		// Cannot convert to nsec for backwards compatibility
{
	raft_leader_heartbeat_timeout___is_set_by_rpc = (leader_heartbeat_timeout_usec > 0);
	nvmeibt_raft_set_effective_leader_heartbeat_timeout_ns(leader_heartbeat_timeout_usec * 1000, 1);
}

void nvmeibt_raft_set_effective_min_election_timeout_factor(int64_t min_election_timeout_factor, bool is_enforcing)
{
	if (is_enforcing || !raft_min_election_timeout_factor___is_set_by_rpc) {
		if (min_election_timeout_factor && raft_min_election_timeout_factor != min_election_timeout_factor) {    // Do not overide with 0
			raft_min_election_timeout_factor = min_election_timeout_factor;
			N_Tf(bghfu3k, "raft_min_election_timeout_factor=@LLD", raft_min_election_timeout_factor);
			nvmeibt_raft_recalc_timeout_constants();
		}
	}
}

void nvmeibt_raft_set_effective_min_election_timeout_factor_USED_ONLY_BY_RPC(int64_t min_election_timeout_factor)
{
	raft_min_election_timeout_factor___is_set_by_rpc = (min_election_timeout_factor > 0);
	nvmeibt_raft_set_effective_min_election_timeout_factor(min_election_timeout_factor, 1);
}

/********************      persist_and_wire_buf      *********************/

enum PERSIST_AND_WIRE_BUF_DIFF {
	PERSIST_AND_WIRE_BUF_DIFF_EQUAL =			0,
	PERSIST_AND_WIRE_BUF_DIFF_TOPO_ONLY =		1,
	PERSIST_AND_WIRE_BUF_DIFF_NON_TOPO =		2,
};

enum PERSIST_AND_WIRE_BUFS_CMP_RES {
	INCOMING_IDX_IS_LOWER,
	INCOMING_IDX_IS_HIGHER,
	IDXS_ARE_EQUAL
};

void persist_and_wire_buf_validate_len(const struct nvmeibt_persist_and_wire_buf *b)
{
	int		b_size = (int)sizeof(*b);
	int		topo = nvmeibt_tlv_get_len(&(b->topo_ctx));
	int		topo_config = nvmeibt_tlv_get_len(&(b->topo_config_ctx));
	int		kafka_mgmt_config = nvmeibt_tlv_get_len(&(b->kafka_mgmt_config_ctx));
	int		raft_members = nvmeibt_tlv_get_len(&(b->raft_members_ctx));
	int		total = persist_and_wire_buf_get_total_len(b);

	N_Tf(rt6j3ls, "total_len=@INT persist_and_wire_buf_hdr_len=@INT topo_len=@INT topo_config_len=@INT kafka_mgmt_config_len=@INT raft_members_len=@INT",
		 total, b_size, topo, topo_config, kafka_mgmt_config, raft_members);
	NTOMA_ASSERT(bud7dk4, total == b_size + topo + topo_config + kafka_mgmt_config + raft_members,
				 "total=@INT persist_and_wire_buf=@INT topo=@INT TOPO_CONFIG=@INT kafka_mgmt_config=@INT raft_members_len=@INT",
				 total, b_size, topo, topo_config, kafka_mgmt_config, raft_members);
}

static void persist_and_wire_recalc_total_len(struct nvmeibt_persist_and_wire_buf *buf)
{
	buf->persist_and_wire_total_len = LE_SWAP32((int)sizeof(*buf) +
								   nvmeibt_tlv_get_len(&(buf->topo_ctx)) +
								   nvmeibt_tlv_get_len(&(buf->topo_config_ctx)) +
								   nvmeibt_tlv_get_len(&(buf->kafka_mgmt_config_ctx)) +
								   nvmeibt_tlv_get_len(&(buf->raft_members_ctx)));
}

static enum PERSIST_AND_WIRE_BUF_DIFF compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(const struct nvmeibt_persist_and_wire_buf *b1, const struct nvmeibt_persist_and_wire_buf *b2)
{
	enum PERSIST_AND_WIRE_BUF_DIFF		rv;
	int64_t								wire_t1, wire_t2, wire_tc1, wire_tc2, wire_kmc1, wire_kmc2, wire_rm1, wire_rm2;

	if (!b1 || !b2) {
		if (b1 || b2) {
			rv = PERSIST_AND_WIRE_BUF_DIFF_NON_TOPO;
		} else {
			rv = PERSIST_AND_WIRE_BUF_DIFF_EQUAL;
		}
		goto out;
	}
	//
	wire_t1 = b1->topo_ctx.tlv_idx;
	wire_t2 = b2->topo_ctx.tlv_idx;
	wire_tc1 = b1->topo_config_ctx.tlv_idx;
	wire_tc2 = b2->topo_config_ctx.tlv_idx;
	wire_kmc1 = b1->kafka_mgmt_config_ctx.tlv_idx;
	wire_kmc2 = b2->kafka_mgmt_config_ctx.tlv_idx;
	wire_rm1 = b1->raft_members_ctx.tlv_idx;
	wire_rm2 = b2->raft_members_ctx.tlv_idx;
	//
	if ((b1->buf_sw_ver != b2->buf_sw_ver) && (b1->buf_sw_ver != 0))
		N_Wf(t7781vs, "b1->buf_sw_ver=@SOFTWARE_VERSION != b2->buf_sw_ver=@SOFTWARE_VERSION", LE_SWAP32(b1->buf_sw_ver), LE_SWAP32(b2->buf_sw_ver));
	if (wire_tc1 != wire_tc2 || wire_kmc1 != wire_kmc2 || wire_rm1 != wire_rm2) {
		rv = PERSIST_AND_WIRE_BUF_DIFF_NON_TOPO;
	} else if (wire_t1 != wire_t2) {
		rv = PERSIST_AND_WIRE_BUF_DIFF_TOPO_ONLY;
	} else {
		rv = PERSIST_AND_WIRE_BUF_DIFF_EQUAL;
	}
out:
	return rv;
}

static enum PERSIST_AND_WIRE_BUFS_CMP_RES compare_incoming_and_my_persist_and_wire_buf_tlv(const struct nvmeibt_persist_and_wire_buf *incoming)
{
	enum PERSIST_AND_WIRE_BUFS_CMP_RES		rv;
	int64_t									t1 = nvmeibt_tlv_get_idx(&(incoming->topo_ctx));
	int64_t									t2 = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed);
	int64_t									rm1 = nvmeibt_tlv_get_idx(&(incoming->raft_members_ctx));
	int64_t									rm2 = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed);
	int64_t									tc1 = nvmeibt_tlv_get_idx(&(incoming->topo_config_ctx));
	int64_t									tc2 = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed);
	int64_t									kmc1 = nvmeibt_tlv_get_idx(&(incoming->kafka_mgmt_config_ctx));
	int64_t									kmc2 = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed);
	bool									is_any_lower, is_any_higher;
	if (nvmeibt_raft_is_leader()) {
		// A leader that is already ahead will reject a vote from an annoying peer
		t2 = max(t2, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_calculated));
		rm2 = max(rm2, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_calculated));
		tc2 = max(tc2, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_calculated));
		kmc2 = max(kmc2, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated));
	}
	is_any_lower = (t1 < t2 || rm1 < rm2 || tc1 < tc2 || kmc1 < kmc2);
	is_any_higher = (t1 > t2 || rm1 > rm2 || tc1 > tc2 || kmc1 > kmc2);
	if (is_any_lower) {
		if (is_any_higher)
			N_Ef(bfhs84j, "Incoming and my RAFT_COMMIT_LIFECYCLE_VAL are not stricktly ordered @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX", t1, t2, tc1, tc2, kmc1, kmc2, rm1, rm2);

		N_Tf(smxkrb6, "Incoming is lower @INT64_TX<@INT64_TX @INT64_TX<@INT64_TX @INT64_TX<@INT64_TX @INT64_TX<@INT64_TX", t1, t2, tc1, tc2, kmc1, kmc2, rm1, rm2);
		rv = INCOMING_IDX_IS_LOWER;
	} else if (is_any_higher) {
		N_Tf(smj8rb6, "Incoming is higher @INT64_TX>@INT64_TX @INT64_TX>@INT64_TX @INT64_TX>@INT64_TX @INT64_TX>@INT64_TX", t1, t2, tc1, tc2, kmc1, kmc2, rm1, rm2);
		rv = INCOMING_IDX_IS_HIGHER;
	} else {
		rv = IDXS_ARE_EQUAL;
	}
	return rv;
}

bool log_if_is_incoming_leader_persist_and_wire_buf_tlv_different_from_follower_committed(const struct nvmeibt_persist_and_wire_buf *incoming)
{
	bool									rv;
	int64_t									t1 = nvmeibt_tlv_get_idx(&(incoming->topo_ctx));
	int64_t									t2 = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed);
	int64_t									tc1 = nvmeibt_tlv_get_idx(&(incoming->topo_config_ctx));
	int64_t									tc2 = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed);
	int64_t									kmc1 = nvmeibt_tlv_get_idx(&(incoming->kafka_mgmt_config_ctx));
	int64_t									kmc2 = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed);
	int64_t									rm1 = nvmeibt_tlv_get_idx(&(incoming->raft_members_ctx));
	int64_t									rm2 = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed);

	if (t1 != t2 || tc1 != tc2 || kmc1 != kmc2 || rm1 != rm2) {
		N_Tf(jwix7dl, "(Incoming_leader != follower_committed) @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX @INT64_TX<>@INT64_TX", t1, t2, tc1, tc2, kmc1, kmc2, rm1, rm2);
		rv = 1;
	} else {
		rv = 0;
	}
	return rv;
}

int nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(const struct nvmeibt_persist_and_wire_buf *src, int8_t tlv_type, char **out_data)
{
	char										*data_ptr = NULL;
	int											wire_out_data_len = 0;

	if (!src) {
		goto out;
	}
	data_ptr = (char *)src->data;	// Override the const
	wire_out_data_len = nvmeibt_tlv_get_len(&(src->topo_ctx));
	if (tlv_type == TLV_TYPE_TOPO_COMPLETE) {
		goto out;
	}
	data_ptr += wire_out_data_len;
	wire_out_data_len = nvmeibt_tlv_get_len(&(src->topo_config_ctx));
	if (tlv_type == TLV_TYPE_TOPO_CONFIG_COMPLETE) {
		goto out;
	}
	data_ptr += wire_out_data_len;
	wire_out_data_len = nvmeibt_tlv_get_len(&(src->kafka_mgmt_config_ctx));
	if (tlv_type == TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE) {
		goto out;
	}
	data_ptr += wire_out_data_len;
	wire_out_data_len = nvmeibt_tlv_get_len(&(src->raft_members_ctx));
	if (tlv_type == TLV_TYPE_RAFT_MEMBERS_COMPLETE) {
		goto out;
	}
	N_Ef(evai3j9, "Unknown tlv_type=@INT", tlv_type);
	data_ptr = NULL;
	wire_out_data_len = 0;
out:
	*out_data = data_ptr;
	return wire_out_data_len;	// 0 for not-found of no data
}

static void init_persist_and_wire_buf(struct nvmeibt_persist_and_wire_buf *buf)
{
	NFIN;
	NTOMA_ASSERT(rva84jk3, buf, "!buf");
	memset(buf, 0, sizeof(*buf));
	// buf must be already allocated large enough
	buf->raft_ctx =              (struct raft_persistency){LE_SWAP64(0LL), LE_SWAP64(0LL), LE_SWAP64(0LL), swap_uuid_LE_BE(&nvmeib_uuid_null_val), swap_uuid_LE_BE(&nvmeib_uuid_null_val), LE_SWAP64(0LL), LE_SWAP64(0LL), LE_SWAP64(0LL), LE_SWAP64(0LL), LE_SWAP64(0LL)};
	buf->topo_ctx =              (struct nvmeibt_wire_type_len_value){0, 0, LE_SWAP8(TLV_TYPE_TOPO_COMPLETE),              0, 0, 0, LE_SWAP64(nvmeibt_offset_and_idx_uninitialized), LE_SWAP64(-1LL)};
	buf->topo_config_ctx =       (struct nvmeibt_wire_type_len_value){0, 0, LE_SWAP8(TLV_TYPE_TOPO_CONFIG_COMPLETE),       0, 0, 0, LE_SWAP64(nvmeibt_offset_and_idx_uninitialized), LE_SWAP64(-1LL)};
	buf->kafka_mgmt_config_ctx = (struct nvmeibt_wire_type_len_value){0, 0, LE_SWAP8(TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE), 0, 0, 0, LE_SWAP64(nvmeibt_offset_and_idx_uninitialized), LE_SWAP64(-1LL)};
	buf->raft_members_ctx =      (struct nvmeibt_wire_type_len_value){0, 0, LE_SWAP8(TLV_TYPE_RAFT_MEMBERS_COMPLETE),      0, 0, 0, LE_SWAP64(nvmeibt_offset_and_idx_uninitialized), LE_SWAP64(-1LL)};
	// calc CRCs
	buf->raft_ctx.raft_ctx_crc = 0;
	buf->raft_ctx.raft_ctx_crc = LE_SWAP32(crc32(0, &buf->raft_ctx, sizeof(buf->raft_ctx)));
	buf->topo_ctx.tlv_crc = 0;
	buf->topo_ctx.tlv_crc = LE_SWAP32(crc32(0, &buf->topo_ctx, sizeof(buf->topo_ctx)));
	buf->topo_config_ctx.tlv_crc = 0;
	buf->topo_config_ctx.tlv_crc = LE_SWAP32(crc32(0, &buf->topo_config_ctx, sizeof(buf->topo_config_ctx)));
	buf->kafka_mgmt_config_ctx.tlv_crc = 0;
	buf->kafka_mgmt_config_ctx.tlv_crc = LE_SWAP32(crc32(0, &buf->kafka_mgmt_config_ctx, sizeof(buf->kafka_mgmt_config_ctx)));
	buf->raft_members_ctx.tlv_crc = 0;
	buf->raft_members_ctx.tlv_crc = LE_SWAP32(crc32(0, &buf->raft_members_ctx, sizeof(buf->raft_members_ctx)));
	//
	persist_and_wire_recalc_total_len(buf);
	buf->buf_sw_ver = LE_SWAP32(nvmeibt_global_get_global()->persistent_toma_software_version);
	NFOUT;
}

static struct nvmeibt_persist_and_wire_buf *alloc_persist_and_wire_buf(size_t req_total_len)
{
	struct nvmeibt_persist_and_wire_buf		*dst;

	NFIN;
	NTOMA_ASSERT(wi9lsk3, req_total_len >= sizeof(*dst), "req_total_len=@SIZE_T<@SIZE_T", req_total_len, sizeof(*dst));
	dst = NNVMEIBT_TOMA_CALLOC(cvsg9ek, 1, req_total_len);
	init_persist_and_wire_buf(dst);
	NFOUT;
	return dst;
}

static int fill_persist_and_wire_tlv_and_data(struct nvmeibt_wire_type_len_value *tlv, char *out_wire_data, int64_t idx, int64_t seq_no, char *in_wire_data, int in_data_len, int8_t tlv_type)
{
	// tlv->tlv_type was already filled at init
	if (in_data_len) {
		memcpy(out_wire_data, in_wire_data, in_data_len);
	}
	memset(tlv, 0, sizeof(*tlv));
	tlv->reserved_1 = 0;
	tlv->reserved_2 = 0;
	tlv->tlv_type = LE_SWAP8(tlv_type);
	tlv->tlv_idx = LE_SWAP64(idx);
	tlv->tlv_len = LE_SWAP32(in_data_len);
	tlv->seq_no = LE_SWAP64(seq_no);
	tlv->tlv_crc = 0;
	tlv->tlv_crc = crc32(0, tlv, sizeof(*tlv));
	tlv->tlv_crc = LE_SWAP32(crc32(tlv->tlv_crc, in_wire_data, in_data_len));
	return in_data_len;
}

struct nvmeibt_persist_and_wire_buf *nvmeibt_raft_generate_persist_and_wire_buf(
	const bool is_incremental,
	unsigned long long current_raft_term,
	unsigned long long last_rx_append_entries_raft_term,
	int64_t kafka_mgmt_zone_number,
	const union nvmeib_uuid *voted_for_raft_member_uuid,
	const union nvmeib_uuid *mgmt_DB_uuid,
	int64_t calculated_append_entries_rep_time_ns,
	int64_t calculated_topo_calc_time_ns,
	uint32_t guaranteed_sw_ver,
	//
	int64_t topo_idx, int64_t topo_seq_no, char *topo_data, int topo_data_len,
	int64_t topo_config_idx, int64_t topo_config_seq_no, char *topo_config_data, int topo_config_data_len,
	int64_t mgmt_config_offset, int64_t mgmt_config_seq_no, char *mgmt_config_data, int mgmt_config_data_len,
	int64_t members_offset, int64_t members_seq_no, char *members_data, int members_data_len)
{
	int										sum_data_len;
	char									*data_ptr;
	struct nvmeibt_persist_and_wire_buf		*dst;
	int8_t									tlv_type_offset;

	NFIN;
	tlv_type_offset = (is_incremental ? TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL - TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE : 0);

	sum_data_len = mgmt_config_data_len + topo_data_len + topo_config_data_len + members_data_len;
	dst = alloc_persist_and_wire_buf(sizeof(*dst) + sum_data_len);
	//
	data_ptr = (char *)dst + sizeof(*dst);
	data_ptr += fill_persist_and_wire_tlv_and_data(&(dst->topo_ctx), data_ptr, topo_idx, topo_seq_no, topo_data, topo_data_len, TLV_TYPE_TOPO_COMPLETE+tlv_type_offset);
	data_ptr += fill_persist_and_wire_tlv_and_data(&(dst->topo_config_ctx), data_ptr, topo_config_idx, topo_config_seq_no, topo_config_data, topo_config_data_len, TLV_TYPE_TOPO_CONFIG_COMPLETE+tlv_type_offset);
	data_ptr += fill_persist_and_wire_tlv_and_data(&(dst->kafka_mgmt_config_ctx), data_ptr, mgmt_config_offset, mgmt_config_seq_no, mgmt_config_data,
												   mgmt_config_data_len, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE+tlv_type_offset);
	data_ptr += fill_persist_and_wire_tlv_and_data(&(dst->raft_members_ctx), data_ptr, members_offset, members_seq_no, members_data,
												   members_data_len, TLV_TYPE_RAFT_MEMBERS_COMPLETE+tlv_type_offset);
	// Set the raft_ctx (usually in the leader), and it travels all the way to the follower's persistence as is
	persist_and_wire_buf_set_current_raft_TERM(dst, current_raft_term, 0);
	persist_and_wire_buf_set_voted_for_and_last_rx_append_entries_raft_TERM(dst, voted_for_raft_member_uuid, last_rx_append_entries_raft_term, 0);
	persist_and_wire_buf_set_kafka_mgmt_zone_number(dst, kafka_mgmt_zone_number, 0);
	persist_and_wire_buf_set_raft_mgmt_DB_uuid(dst, mgmt_DB_uuid, 0);
	persist_and_wire_buf_set_raft_calculated_append_entries_rep_time_ns(dst, calculated_append_entries_rep_time_ns, 0);
	persist_and_wire_buf_set_raft_calculated_topo_calc_time_ns(dst, calculated_topo_calc_time_ns, 0);
	persist_and_wire_buf_set_guaranteed_software_version(dst, guaranteed_sw_ver, 0);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(dst, 1);
	persist_and_wire_recalc_total_len(dst);
	//
	NFOUT;
	return dst;
}

int nvmeibt_raft_leader_copy_committed_persist_and_wire_buf_sections_into_separate_to_commit_bufs(void)
{
	struct nvmeibt_persist_and_wire_buf		*src_buf;
	int										section_buf_len;
	char									*section_buf;

	NFIN;
	src_buf = my_raft_global.follower_to_commit_persist_and_wire_buf_full;

	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(src_buf, TLV_TYPE_TOPO_COMPLETE, &section_buf);
	if (section_buf_len) {
		N_Tf(rxcva3J, "Copy topo");
		NNVMEIBT_BUF_RESIZE(o3mgias, &(my_raft_global.leader_to_commit_wire_topo_complete), (size_t)section_buf_len);
		memcpy(my_raft_global.leader_to_commit_wire_topo_complete.data_buf, section_buf, section_buf_len);
	}
	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(src_buf, TLV_TYPE_TOPO_CONFIG_COMPLETE, &section_buf);
	if (section_buf_len) {
		N_Tf(vbdkllr, "Copy topo_config");
		NNVMEIBT_BUF_RESIZE(9vkkewj, &(my_raft_global.leader_to_commit_wire_topo_config_complete), (size_t)section_buf_len);
		memcpy(my_raft_global.leader_to_commit_wire_topo_config_complete.data_buf, section_buf, section_buf_len);
	}
	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(src_buf, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, &section_buf);
	if (section_buf_len) {
		N_Tf(cyugsiu, "Copy kafka_mgmt_config");
		NNVMEIBT_BUF_RESIZE(abwiwn2, &(my_raft_global.leader_to_commit_wire_kafka_mgmt_config_complete), (size_t)section_buf_len);
		memcpy(my_raft_global.leader_to_commit_wire_kafka_mgmt_config_complete.data_buf, section_buf, section_buf_len);
	}
	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(src_buf, TLV_TYPE_RAFT_MEMBERS_COMPLETE, &section_buf);
	if (section_buf_len) {
		N_Tf(hduksi3, "Copy raft_members");
		NNVMEIBT_BUF_RESIZE(oudf3xy, &(my_raft_global.leader_to_commit_wire_raft_members_complete), (size_t)section_buf_len);
		memcpy(my_raft_global.leader_to_commit_wire_raft_members_complete.data_buf, section_buf, section_buf_len);
	}
	NFOUT;
	return 0;
}

static void persist_and_wire_buf_copy_data_to_section(struct nvmeibt_wire_type_len_value *dst_wire_ctx, const struct nvmeibt_wire_type_len_value *old_wire_ctx, const struct nvmeibt_wire_type_len_value *upd_wire_ctx,
													  char **dst_data_ptr, char **old_data_ptr, const char **upd_data_ptr)
{
	int		upd_len = nvmeibt_tlv_get_len(upd_wire_ctx);
	int		old_len = nvmeibt_tlv_get_len(old_wire_ctx);

	if (upd_len || (nvmeibt_tlv_get_idx(upd_wire_ctx) != nvmeibt_tlv_get_idx(old_wire_ctx))) {
		memcpy(*dst_data_ptr, *upd_data_ptr, upd_len);
		*dst_wire_ctx = *upd_wire_ctx;
	} else {
		memcpy(*dst_data_ptr, *old_data_ptr, old_len);	// might be that old_len==0
		*dst_wire_ctx = *old_wire_ctx;
	}
	// Now copy wire-->wire format

	*upd_data_ptr += upd_len;
	*old_data_ptr += old_len;
	*dst_data_ptr += nvmeibt_tlv_get_len(dst_wire_ctx);
}


/*
 * @dst_wire_ctx: Destination TLV header (can be NULL for size calculation only)
 * @old_wire_ctx: Old TLV header (can be NULL for initial complete case)
 * @upd_wire_ctx: Update TLV header (complete or incremental)
 * @dst_data_ptr: Pointer to destination data pointer (can be NULL for size calculation). When provided, caller must allocate just enough space for the data.
 * @old_data_ptr: Pointer to old data pointer (can be NULL)
 * @upd_data_ptr: Pointer to update data pointer (must be provided)
 *
 * Returns: Size of the resulting data, or -1 on error
 */
static int __attribute__((unused)) persist_and_wire_buf_calculate_and_merge_data_to_section(struct nvmeibt_wire_type_len_value *dst_wire_ctx, const struct nvmeibt_wire_type_len_value *old_wire_ctx, const struct nvmeibt_wire_type_len_value *upd_wire_ctx,
								   char **dst_data_ptr, char **old_data_ptr, const char **upd_data_ptr)
{
	int			upd_len = 0;
	int			old_len = 0;
	int			total_size = -1;
	int8_t		upd_tlv_type = TLV_TYPE_UNKNOWN;

	NFIN;
	// Input validation
	if (!upd_wire_ctx || !upd_data_ptr) {
		N_Ef(asd923k, "upd_wire_ctx or upd_data_ptr is NULL");
		goto out;
	}

	// Get lengths and type
	upd_len = nvmeibt_tlv_get_len(upd_wire_ctx);
	old_len = old_wire_ctx ? nvmeibt_tlv_get_len(old_wire_ctx) : 0;
	upd_tlv_type = nvmeibt_tlv_get_type(upd_wire_ctx);

	// Complete types - just use persist_and_wire_buf_copy_data_to_section
	if (upd_tlv_type == TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE ||
		upd_tlv_type == TLV_TYPE_TOPO_CONFIG_COMPLETE ||
		upd_tlv_type == TLV_TYPE_RAFT_MEMBERS_COMPLETE ||
		upd_tlv_type == TLV_TYPE_TOPO_COMPLETE) {

		if (dst_wire_ctx) {
			persist_and_wire_buf_copy_data_to_section(dst_wire_ctx, old_wire_ctx, upd_wire_ctx,
													  dst_data_ptr, old_data_ptr, upd_data_ptr);
		}
		// Calculate total size
		total_size = upd_len;
		goto out;
	}

	// Process incremental types
	switch (upd_tlv_type) {
	case TLV_TYPE_TOPO_INCREMENTAL: {
		struct nvmeibt_topology_serialized_topo_header	*old_wire_header = NULL;
		struct nvmeibt_topology_serialized_topo_header	*upd_wire_header = NULL;
		struct nvmeibt_topology_serialized_topo_header	*dst_wire_header = NULL;
		struct nvmeibt_topology_serialized_topo_header	old_serialized_header;
		struct nvmeibt_topology_serialized_topo_header	upd_serialized_header;
		struct nvmeibt_praid_serialized_topo			*old_wire_praid = NULL;
		struct nvmeibt_praid_serialized_topo			*upd_wire_praid = NULL;
		struct nvmeibt_praid_serialized_topo			*dst_wire_praid = NULL;
		int												data_len = 0;
		int												n_praids_in_result = 0;
		int												n_segs_in_result = 0;
		int												i = 0;

		// Validation: incremental topo requires old complete topo
		if (!old_wire_ctx || old_len == 0 || nvmeibt_tlv_get_type(old_wire_ctx) != TLV_TYPE_TOPO_COMPLETE) {
			N_Ef(wer923k, "Incremental topo update requires old complete topo to merge with. old_wire_ctx=@PTR old_len=@INT tlv_type=@INT8_TD",
				old_wire_ctx, old_len, old_wire_ctx ? nvmeibt_tlv_get_type(old_wire_ctx) : (int8_t)-1);
			nvmeibt_abort(ES_FATAL);
			total_size = -1;
			goto out;
		}

		// The persist_and_wire_buf carries 4 TLV sections (TOPO, TOPO_CONFIG,
		// KAFKA_MGMT_CONFIG, RAFT_MEMBERS). When the leader generates an incremental
		// wire buf, only some sections may have changed. A section whose data is
		// unaffected will have upd_len==0 — in that case, keep the old data as-is.
		if (upd_len == 0) {
			if (dst_wire_ctx) {
				// This function handles upd_len==0 case, by copying old data
				persist_and_wire_buf_copy_data_to_section(dst_wire_ctx, old_wire_ctx, upd_wire_ctx,
														  dst_data_ptr, old_data_ptr, upd_data_ptr);
			}
			total_size = old_len;
			goto out;
		}

		// Parse headers and convert from wire format
		old_wire_header = (struct nvmeibt_topology_serialized_topo_header *)*old_data_ptr;
		upd_wire_header = (struct nvmeibt_topology_serialized_topo_header *)*upd_data_ptr;
		nvmeibt_topology_convert_header_le_be(old_wire_header, &old_serialized_header);
		nvmeibt_topology_convert_header_le_be(upd_wire_header, &upd_serialized_header);

		// Initialize pointers to praid data
		old_wire_praid = (struct nvmeibt_praid_serialized_topo *)(old_wire_header + 1);
		upd_wire_praid = (struct nvmeibt_praid_serialized_topo *)(upd_wire_header + 1);

		// Start with topo header size for data calculation
		data_len = sizeof(struct nvmeibt_topology_serialized_topo_header);

		// If generating output, prepare the TLV and topo headers
		if (dst_wire_ctx && dst_data_ptr) {
			// Start from the update TLV (carries the newer tlv_idx and seq_no)
			*dst_wire_ctx = *upd_wire_ctx;

			// Copy old topo header to output data area
			dst_wire_header = (struct nvmeibt_topology_serialized_topo_header *)*dst_data_ptr;
			memcpy(dst_wire_header, old_wire_header, sizeof(*old_wire_header));
			dst_wire_praid = (struct nvmeibt_praid_serialized_topo *)(dst_wire_header + 1);
		}

		// For each old praid, scan the incremental buffer for a UUID match.
		// We do not assume same ordering between old and incremental buffers,
		// since hash table iteration order may differ across serializations
		// (e.g., due to add/remove/resize). The incremental set is expected to
		// be small, so O(n*m) is acceptable.
		for (i = 0; i < old_serialized_header.praids_num; i++) {
			struct nvmeibt_praid_serialized_topo	*wire_praid_to_copy;
			struct nvmeibt_praid_serialized_topo	old_serialized_praid, upd_serialized_praid;
			struct nvmeibt_praid_serialized_topo	*cur_upd_wire_praid;
			int										old_segs_size;
			int										upd_segs_size = 0;
			int										praid_total_size;
			bool									use_upd_praid = false;

			// Convert current old praid to working format
			nvmeibt_praid_convert_topo_le_be(old_wire_praid, &old_serialized_praid, TOMA_SW_COMPATIBILITY_VER);
			old_segs_size = nvmeibt_praid_wire_get_n_segs(old_wire_praid) * sizeof(struct nvmeibt_serialized_seg_leader_topo);

			// Scan incremental buffer for a praid with matching UUID
			cur_upd_wire_praid = upd_wire_praid;
			for (int j = 0; j < upd_serialized_header.praids_num; j++) {
				nvmeibt_praid_convert_topo_le_be(cur_upd_wire_praid, &upd_serialized_praid, TOMA_SW_COMPATIBILITY_VER);
				upd_segs_size = nvmeibt_praid_wire_get_n_segs(cur_upd_wire_praid) * sizeof(struct nvmeibt_serialized_seg_leader_topo);

				if (ARE_UUID_EQ(&old_serialized_praid.uuid, &upd_serialized_praid.uuid)) {
					if (old_serialized_praid.topo_idx_updated < upd_serialized_praid.topo_idx_updated) {
						use_upd_praid = true;
						wire_praid_to_copy = cur_upd_wire_praid;

						N_Tf(asd82jk, "Found matching praid=@UUID_LE in incremental, segs=@INT, old topo_idx_updated=@INT64_TX < upd topo_idx_updated=@INT64_TX",
							&old_serialized_praid.uuid, nvmeibt_praid_wire_get_n_segs(cur_upd_wire_praid),
							old_serialized_praid.topo_idx_updated, upd_serialized_praid.topo_idx_updated);
					}
					break;
				}
				cur_upd_wire_praid = (struct nvmeibt_praid_serialized_topo *)((char *)cur_upd_wire_praid + sizeof(*cur_upd_wire_praid) + upd_segs_size);
			}

			if (!use_upd_praid) {
				wire_praid_to_copy = old_wire_praid;
			}

			// Update counters
			n_praids_in_result++;
			n_segs_in_result += nvmeibt_praid_wire_get_n_segs(wire_praid_to_copy);

			// Calculate size and update data length
			praid_total_size = sizeof(*wire_praid_to_copy) + (use_upd_praid ? upd_segs_size : old_segs_size);
			data_len += praid_total_size;

			// Copy to output if generating
			if (dst_wire_praid) {
				memcpy(dst_wire_praid, wire_praid_to_copy, praid_total_size);
				dst_wire_praid = (struct nvmeibt_praid_serialized_topo *)
					((char *)dst_wire_praid + praid_total_size);
			}

			// Advance to next old praid
			old_wire_praid = (struct nvmeibt_praid_serialized_topo *)
				((char *)old_wire_praid + sizeof(*old_wire_praid) + old_segs_size);
		}

		// Calculate final total size
		total_size = data_len;

		// Finalize output TLV and topo headers with actual counts
		if (dst_wire_ctx) {
			// The merged result is a complete topo; override type and length, recalculate CRC
			dst_wire_ctx->tlv_type = LE_SWAP8(TLV_TYPE_TOPO_COMPLETE);
			dst_wire_ctx->tlv_len = LE_SWAP32(data_len);
			dst_wire_ctx->tlv_crc = LE_SWAP32(crc32(0, dst_wire_ctx, sizeof(*dst_wire_ctx)));

			// Update topo header within the data
			if (dst_wire_header) {
				dst_wire_header->topo_len = LE_SWAP32(data_len);
				dst_wire_header->praids_num = LE_SWAP32(n_praids_in_result);
			}

			// Advance destination data pointer
			if (dst_data_ptr) {
				*dst_data_ptr += data_len;
			}
		}

		// Advance source data pointers
		*old_data_ptr += old_len;
		*upd_data_ptr += upd_len;

		break;
	}

	case TLV_TYPE_KAFKA_MGMT_CONFIG_INCREMENTAL:
		N_Ef(kfk923j, "KAFKA_MGMT_CONFIG_INCREMENTAL not implemented yet - using standard copy");
		break;

	case TLV_TYPE_TOPO_CONFIG_INCREMENTAL:
		N_Ef(tpc834k, "TOPO_CONFIG_INCREMENTAL not implemented yet - using standard copy");
		break;

	case TLV_TYPE_RAFT_MEMBERS_INCREMENTAL:
		N_Ef(rft923m, "RAFT_MEMBERS_INCREMENTAL not implemented yet - using standard copy");
		break;

	default:
		N_Ef(def892k, "Unexpected incremental TLV type=@INT8_TD", upd_tlv_type);
		goto out;
	}

out:
	NFOUT;
	return total_size;
}

// returns a newly allocated struct where the new-upd takes presidence (whenever it carries a value)
// Always returns a ptr to a valid usable struct (possibly with no data)
// The old buf is freed / reused
static struct nvmeibt_persist_and_wire_buf *realloc_and_upd_follower_persist_and_wire_bufs_with_incoming_data(
	struct nvmeibt_persist_and_wire_buf *old, const struct nvmeibt_persist_and_wire_buf *upd, bool is_with_raft_log)
{
	int										dst_data_len;
	int										upd_total_len;
	struct nvmeibt_persist_and_wire_buf		*dst;
	const char								*upd_data_ptr;
	char									*old_data_ptr;
	char									*dst_data_ptr;
	enum PERSIST_AND_WIRE_BUF_DIFF			wire_buffs_diff;
	bool									is_raft_ctx_eq;

	NFIN;
	// upd always exists, it is an embedded struct in the incoming msg
	upd_total_len = persist_and_wire_buf_get_total_len(upd);
	if (!old) {
		dst = alloc_persist_and_wire_buf(upd_total_len);
		if (is_with_raft_log) {
			memcpy(dst, upd, upd_total_len);
		} else {
			dst->raft_ctx = upd->raft_ctx;
		}
		is_raft_ctx_eq = 1;
		goto out;
	}
	// We had both old and upd
	wire_buffs_diff = compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(old, upd);
	is_raft_ctx_eq = (memcmp(&(old->raft_ctx), &(upd->raft_ctx), sizeof(old->raft_ctx)) == 0);
	if (!is_with_raft_log || wire_buffs_diff == PERSIST_AND_WIRE_BUF_DIFF_EQUAL) {
		// Probably REQ_VOTE or APPEND_ENTRIES with no data. Only update the raft_ctx, and do not touch the TLVs and their data
		dst = old;
		goto out;
	}
	// Non raft_ctx requires update
	upd_data_ptr = upd->data;
	if ((wire_buffs_diff == PERSIST_AND_WIRE_BUF_DIFF_TOPO_ONLY) && (nvmeibt_tlv_get_len(&(upd->topo_ctx)) == nvmeibt_tlv_get_len(&(old->topo_ctx)))) {
		// A pretty common case. Only the topo details changed, so we can store the incoming topo over the same memory
		dst = old;
		dst_data_ptr = (char *)dst + sizeof(*dst);
		old_data_ptr = (char *)old + sizeof(*old);
		persist_and_wire_buf_copy_data_to_section(&(dst->topo_ctx), &(old->topo_ctx), &(upd->topo_ctx), &dst_data_ptr, &old_data_ptr, &upd_data_ptr);
		goto out;
	}
	// calc dst_sum_len
	dst_data_len = (nvmeibt_tlv_get_len(nvmeibt_tlv_get_len(&(upd->topo_ctx)) ? &(upd->topo_ctx) : &(old->topo_ctx)) +
					nvmeibt_tlv_get_len(nvmeibt_tlv_get_len(&(upd->topo_config_ctx)) ? &(upd->topo_config_ctx) : &(old->topo_config_ctx)) +
					nvmeibt_tlv_get_len(nvmeibt_tlv_get_len(&(upd->kafka_mgmt_config_ctx)) ? &(upd->kafka_mgmt_config_ctx) : &(old->kafka_mgmt_config_ctx)) +
					nvmeibt_tlv_get_len(nvmeibt_tlv_get_len(&(upd->raft_members_ctx)) ? &(upd->raft_members_ctx) : &(old->raft_members_ctx)));
	// Allocate new dst according to size and copy into the new dst
	dst = alloc_persist_and_wire_buf(sizeof(*dst) + dst_data_len);
	is_raft_ctx_eq = 0; // we must copy the raft_ctx to the new buf
	dst_data_ptr = (char *)dst + sizeof(*dst);
	old_data_ptr = (char *)old + sizeof(*old);

	persist_and_wire_buf_copy_data_to_section(&(dst->topo_ctx), &(old->topo_ctx), &(upd->topo_ctx), &dst_data_ptr, &old_data_ptr, &upd_data_ptr);
	persist_and_wire_buf_copy_data_to_section(&(dst->topo_config_ctx), &(old->topo_config_ctx), &(upd->topo_config_ctx), &dst_data_ptr, &old_data_ptr, &upd_data_ptr);
	persist_and_wire_buf_copy_data_to_section(&(dst->kafka_mgmt_config_ctx), &(old->kafka_mgmt_config_ctx), &(upd->kafka_mgmt_config_ctx), &dst_data_ptr, &old_data_ptr, &upd_data_ptr);
	persist_and_wire_buf_copy_data_to_section(&(dst->raft_members_ctx), &(old->raft_members_ctx), &(upd->raft_members_ctx), &dst_data_ptr, &old_data_ptr, &upd_data_ptr);
	persist_and_wire_recalc_total_len(dst);
	NNVMEIBT_TOMA_FREE(iqwv3j4, old);	// We allocated a new one and not reused
out:
	dst->buf_sw_ver = upd->buf_sw_ver;
	if (!is_raft_ctx_eq) {
		dst->raft_ctx = upd->raft_ctx;
	}
	NFOUT;
	return dst;
}

void raft_leader_regenerate_the_to_commit_persist_and_wire_bufs_as_needed(void)
{
	NFIN;
	if (!(nvmeibt_global_get_global()->is_update_csv_of_config_and_topo_required)) {
		goto out;
	}
	NNVMEIBT_TOMA_FREE(ikdm49s, my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete);
	my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete = nvmeibt_raft_generate_persist_and_wire_buf(
		false,
		nvmeibt_raft_get_current_term(),
		nvmeibt_raft_get_current_term(),
		nvmeibt_kafka_get_kafka_mgmt_zone_number(),
		&(my_raft_global.my_member->uuid),	// The voted_for_raft_member in the follower's persistence
		nvmeibt_global_get_mgmt_DB_uuid(),
		nvmeibt_raft_leader_get_append_entries_rep_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_leader_get_topo_calc_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_get_guaranteed_sw_ver(),
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_topo_complete.data_buf, my_raft_global.leader_to_commit_wire_topo_complete.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_topo_config_complete.data_buf, my_raft_global.leader_to_commit_wire_topo_config_complete.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_kafka_mgmt_config_complete.data_buf, my_raft_global.leader_to_commit_wire_kafka_mgmt_config_complete.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_to_commit), RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_to_commit), my_raft_global.leader_to_commit_wire_raft_members_complete.data_buf, my_raft_global.leader_to_commit_wire_raft_members_complete.buf_len);
	NNVMEIBT_TOMA_FREE(6vbwi4k, my_raft_global.leader_to_commit_persist_and_wire_buf_topo_only_complete);
	my_raft_global.leader_to_commit_persist_and_wire_buf_topo_only_complete = nvmeibt_raft_generate_persist_and_wire_buf(
		false,
		nvmeibt_raft_get_current_term(),
		nvmeibt_raft_get_current_term(),
		nvmeibt_kafka_get_kafka_mgmt_zone_number(),
		&(my_raft_global.my_member->uuid),	// The voted_for_raft_member in the follower's persistence
		nvmeibt_global_get_mgmt_DB_uuid(),
		nvmeibt_raft_leader_get_append_entries_rep_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_leader_get_topo_calc_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_get_guaranteed_sw_ver(),
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_topo_complete.data_buf, my_raft_global.leader_to_commit_wire_topo_complete.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit), -1, NULL, 0,
		RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_to_commit), -1, NULL, 0,
		RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_to_commit), RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_to_commit), my_raft_global.leader_to_commit_wire_raft_members_complete.data_buf, my_raft_global.leader_to_commit_wire_raft_members_complete.buf_len);
	NNVMEIBT_TOMA_FREE(sk1lams, my_raft_global.leader_to_commit_persist_and_wire_buf_full_incremental);
	my_raft_global.leader_to_commit_persist_and_wire_buf_full_incremental = nvmeibt_raft_generate_persist_and_wire_buf(
		true,
		nvmeibt_raft_get_current_term(),
		nvmeibt_raft_get_current_term(),
		nvmeibt_kafka_get_kafka_mgmt_zone_number(),
		&(my_raft_global.my_member->uuid),	// The voted_for_raft_member in the follower's persistence
		nvmeibt_global_get_mgmt_DB_uuid(),
		nvmeibt_raft_leader_get_append_entries_rep_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_leader_get_topo_calc_time_ns_for_persist_and_wire_buf(),
		nvmeibt_raft_get_guaranteed_sw_ver(),
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_topo_incremental.data_buf, my_raft_global.leader_to_commit_wire_topo_incremental.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_topo_config_incremental.data_buf, my_raft_global.leader_to_commit_wire_topo_config_incremental.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_to_commit), -1, my_raft_global.leader_to_commit_wire_kafka_mgmt_config_incremental.data_buf, my_raft_global.leader_to_commit_wire_kafka_mgmt_config_incremental.buf_len,
		RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_to_commit), RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_to_commit), my_raft_global.leader_to_commit_wire_raft_members_incremental.data_buf, my_raft_global.leader_to_commit_wire_raft_members_incremental.buf_len);
	raft_leader_reset_counters_upon_last_LOG_change();
	nvmeibt_global_get_global()->is_update_csv_of_config_and_topo_required = false;
out:
	NFOUT;
}

/**************************        members        *****************************/

struct all_members_wire_buf_ctx {
	int								n_raft_members;
	int								filler_for_align_8;
	struct mm_raft_member_conf		members[0];
} __attribute__((__packed__));

static void raft_reset_member_ctx(struct nvmeibt_raft_member *member)
{
	NFIN;
	if (member) {
		member->last_local_serialization_version = NVMEIBT_NOT_INITIALIZED_SER_VER;
	}
	NFOUT;
}

static unsigned long long raft_member_get_last_local_serialization_version(struct nvmeibt_raft_member *member)
{
	return (member ? member->last_local_serialization_version : NVMEIBT_NOT_INITIALIZED_SER_VER);
}

static void nvmeibt_raft_leader_generate_member_wire_from_member(struct nvmeibt_raft_member *member)
{
	struct mm_raft_member_conf		tmp_conf = {0};
	NFIN;
	nvmeibt_strlcpy(tmp_conf.eyecatcher, "MMB", sizeof(tmp_conf.eyecatcher));
	nvmeibt_strlcpy(tmp_conf.hostname, nvmeibt_raft_member_name(member), sizeof(tmp_conf.hostname));
	tmp_conf.uuid = *nvmeibt_raft_member_id(member);
	tmp_conf.kafka_offset = member->kafka_offset;
	nvmeibt_raft_member_conf_convert_le_be(&(member->this_member_leader_serialized_wire_buf), &tmp_conf);
	NFOUT;
}

void nvmeibt_raft_leader_generate_leader_to_commit_wire_raft_members_buf(void)
{
	struct nvmeibt_Buf								*wire_conf_buf;
	struct nvmeibt_raft_member						*member;
	struct all_members_wire_buf_ctx					*members_wire_buf;
	int												i = 0;
	size_t											required_size;

	NFIN;
	wire_conf_buf = &(my_raft_global.leader_to_commit_wire_raft_members_complete);
	required_size = sizeof(struct all_members_wire_buf_ctx) + my_raft_global.n_raft_members * sizeof(struct mm_raft_member_conf) + sizeof(EYECATCHER_CNF_END);
	NNVMEIBT_BUF_RESIZE(gso0snh, wire_conf_buf, required_size);
	memset(wire_conf_buf->data_buf, 0, wire_conf_buf->buf_len);
	members_wire_buf = (struct all_members_wire_buf_ctx *)(wire_conf_buf->data_buf);
	// Fill in
	members_wire_buf->n_raft_members = LE_SWAP32(my_raft_global.n_raft_members);
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		// Note that this is not ordered in any way. Those members are in!
		members_wire_buf->members[i++] = member->this_member_leader_serialized_wire_buf;
	}
	nvmeibt_strlcpy((char *)&(members_wire_buf->members[i]), EYECATCHER_CNF_END, wire_conf_buf->buf_len - (int)((char *)&(members_wire_buf->members[i]) - (char *)(wire_conf_buf->data_buf)));
	N_Tf(5bcjs82, "New conf ver=@KAFKA_OFST n_members=@INT len=@SIZE_T", RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_to_commit), LE_SWAP32(members_wire_buf->n_raft_members), wire_conf_buf->buf_len);
	NFOUT;
}

void nvmeibt_raft_link_member_to_node(struct nvmeibt_raft_member *member, struct nvmeibt_node *node, const union nvmeib_uuid *uuid)
{
	NFIN;
	// If any of member or node are NULL, then we require a value in the arg uuid
	if (!member) {
		member = nvmeibt_raft_get_member_by_id(uuid);
	}
	if (!node) {
		node = nvmeibt_node_get_node_by_id(uuid);
	}
	if (node) {
		node->raft_member = member;
	}
	if (member) {
		member->its_node = node;
	}
	NFOUT;
}

void nvmeibt_raft_unlink_member_from_node(struct nvmeibt_raft_member *member, struct nvmeibt_node *node)
{
	NFIN;
	if (!member) {
		member = nvmeibt_node_get_raft_member(node);
	}
	if (!node) {
		node = nvmeibt_raft_member_get_node(member);
	}
	if ((node && (node->raft_member != member)) || (member && (member->its_node != node))) {
		N_Ef(xcvsghw, "member=@UUID_LE node=@UUID_LE", nvmeibt_raft_member_id(member), nvmeibt_node_UUID(node));
	}
	if (node) {
		node->raft_member = NULL;
	}
	if (member) {
		member->its_node = NULL;
	}
	NFOUT;
}

static void raft_convert_to_follower(const union nvmeib_uuid *voted_for_uuid, const union nvmeib_uuid *leader_uuid);

inline static bool is_raft_majority(void)
{
	return ((my_raft_global.n_peers_voted_for_me * 2) > my_raft_global.n_raft_active_members);
}

static void convert_to_follower_if_majority_is_lost(void)
{
	if (!is_raft_majority() && (my_raft_global.role != RAFT_ROLE_FOLLOWER))
		raft_convert_to_follower(nvmeibt_raft_get_voted_for_uuid(), raft_get_leader_node_uuid());
}

void nvmeibt_raft_add_member(char *hostname, int n_raft_members_total_before_add_del, const union nvmeib_uuid *uuid, bool is_incremental_add_fr_mgmt, int64_t kafka_offset, int config_tag)
{
	struct nvmeibt_raft_member		*member;
	bool							is_me;

	NFIN;
	member = nvmeibt_raft_get_member_by_id(uuid);
	if (member) {
		if (is_incremental_add_fr_mgmt) {
			N_Wf(fhusk3m, "member=@STR already in the game", hostname);
		} else {
			N_Tf(ysbwkl2, "member=@STR already in the game", hostname);
		}
		goto out;
	}
	is_me = (strncmp(nvmeibt_get_my_hostname(), hostname, NVMEIB_HOST_NAME_LEN) == 0);
	if (is_incremental_add_fr_mgmt) {
		if (n_raft_members_total_before_add_del != my_raft_global.n_raft_members) {
			N_Wf(4cnautm, "n_raft_members_total_before_add_del=@INT AND n_raft_members=@INT", n_raft_members_total_before_add_del, my_raft_global.n_raft_members);
			goto out;
		}
		// Try to add a member in two cases
		// 1. I am a leader
		// 2. I have 0 members (I.e., possibly, I am the first member in this raft domain)
		if (nvmeibt_raft_is_leader()) {
			N_Tf(ashs83k, "I am the leader, OK to try and add a member");
		} else if (my_raft_global.n_raft_members == 0) {
			if (is_me) {
				N_Tf(6ndfipk, "I am the first member, OK to try and add a member");
			} else {
				N_Tf(vdux7fu, "I thought that I am the first member, but @STR is added. Ignoring, and awaiting for leader's messages", hostname);
				goto out;
			}
		}
	}
	member = NNVMEIBT_TOMA_CALLOC(trvsau2, 1, sizeof(*member));
	nvmeibt_strlcpy(member->hostname, hostname, sizeof(member->hostname));
	member->uuid = *uuid;
	member->urn_uuid = nvmeibt_union_uuid_to_urn_uuid(uuid);
	member->kafka_offset = kafka_offset;
	//
	nvmeib_hash_add_uuid(my_raft_global.raft_members_hash_by_uuid, nvmeibt_raft_member_id(member), member);
	(my_raft_global.n_raft_members)++;
	(my_raft_global.n_raft_active_members)++;
	//
	nvmeibt_raft_link_member_to_node(member, NULL, uuid);
	//
	member->is_me = is_me;
	if (member->is_me) {
		my_raft_global.my_member = member;
	}
	nvmeibt_raft_leader_generate_member_wire_from_member(member);
	raft_reset_member_ctx(member);
	N_Tf(d4v39sa, "Added member hostname=@STR n_members_after=@INT uuid=@UUID_LE @KAFKA_OFST",
		 nvmeibt_raft_member_name(member), nvmeib_hash_get_n_elements(my_raft_global.raft_members_hash_by_uuid), nvmeibt_raft_member_id(member), kafka_offset);
	convert_to_follower_if_majority_is_lost();
	// If I am the first and only member, then convert to candidate that starts from the committed members_list
out:
	if (member) {
		member->config_tag = config_tag;
	}
	NFOUT;
}

void nvmeibt_raft_del_member(char *hostname, int n_raft_members_total_before_add_del, const union nvmeib_uuid *uuid, bool is_incremental_del_fr_mgmt, int64_t kafka_offset)
{
	struct nvmeibt_raft_member		*member;
	bool							is_me;

	NFIN;
	member = nvmeibt_raft_get_member_by_id(uuid);
	is_me = raft_is_my_uuid(uuid);
	if (!member) {
		N_Tf(vhs7ej9, "member=@STR already NOT in the game", hostname);
		goto out;
	}
	if (is_incremental_del_fr_mgmt) {
		if (n_raft_members_total_before_add_del != my_raft_global.n_raft_members) {
			N_Wf(3usjdiv, "n_raft_members_total_before_add_del=@INT AND n_raft_members=@INT", n_raft_members_total_before_add_del, my_raft_global.n_raft_members);
			goto out;
		}
		if (nvmeibt_raft_is_leader()) {
			N_Tf(zypgir3, "I am the leader, OK to try and del a member");
		} else {
			N_Tf(bdut7si, "I am not a leader. got incremental del(@STR). Ignoring", hostname);
			goto out;
		}
	}
	nvmeib_hash_delete_uuid(my_raft_global.raft_members_hash_by_uuid, nvmeibt_raft_member_id(member));
	--(my_raft_global.n_raft_members);
	if (!member->is_ignored)
		--(my_raft_global.n_raft_active_members);
	if (member->is_vote_valid)
		--(my_raft_global.n_peers_voted_for_me);
	if (is_me) {
		my_raft_global.my_member = NULL;
	}
	if (raft_is_leader_uuid(uuid)) {
		nvmeibt_raft_set_leader_uuid(NULL);
	}
	if (raft_is_voted_for_uuid(uuid)) {
		nvmeibt_raft_set_voted_for_uuid(NULL);
	}
	nvmeibt_raft_unlink_member_from_node(member, NULL);
	N_Tf(vnhve8w, "Del member hostname=@STR n_members_after=@INT uuid=@UUID_LE @KAFKA_OFST",
		 nvmeibt_raft_member_name(member), nvmeib_hash_get_n_elements(my_raft_global.raft_members_hash_by_uuid), nvmeibt_raft_member_id(member), kafka_offset);
	convert_to_follower_if_majority_is_lost();
	NNVMEIBT_TOMA_FREE(cvgsuyg, member);
out:
	NFOUT;
}

void nvmeibt_raft_del_all_members_at_exit(void)
{
	struct nvmeibt_raft_member	*member;
	struct nvmeib_hash_table	*h = my_raft_global.raft_members_hash_by_uuid;
	NFIN;
	NVMEIB_HASH_FOREACH(member, h) {
		//nvmeib_hash_delete_uuid(h, nvmeibt_raft_member_id(member));
		//nvmeibt_raft_unlink_member_from_node(member, NULL);
		N_Tf(__AUTOID__, "Del member hostname=@STR", nvmeibt_raft_member_name(member));
		NNVMEIBT_TOMA_FREE(__AUTOID__, member);
	}
	my_raft_global.n_raft_members = my_raft_global.n_raft_active_members = my_raft_global.n_peers_voted_for_me = 0;
	NFOUT;
}


static struct nvmeibt_raft_member *raft_get_member_by_name(char *name)
{
	struct nvmeibt_raft_member	*member;
	bool						is_found = 0;

	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		if (!strcmp(nvmeibt_raft_member_name(member), name)) {
			is_found = 1;
			break;
		}
	}
	if (!is_found) {
		N_Tf(vnekf21, "Member='@STR' not found", name);
	}
	return (is_found ? member : NULL);
}

int nvmeibt_raft_ignore_member(char *hostname)
{
	struct nvmeibt_raft_member		*member;

	member = raft_get_member_by_name(hostname);
	if (member && !member->is_ignored) {
		member->is_ignored = 1;
		my_raft_global.n_raft_active_members--;
		N_Tf(vhs7wy3, "member=@STR marked as ignored", hostname);
		return 0;
	} else {
		N_Tf(vhs7wy6, "member=@STR already NOT in the game", hostname);
		return -1;
	}
}

#define DUMP_RAFT_MEMBER_CONF(name, _i, _raft_member)	({																										\
	struct mm_raft_member_conf		*mmb = _raft_member;																										\
	N_Tf(name, "member[@INT]: eyecatcher=@STR hostname=@STR uuid=@UUID_LE @KAFKA_OFST", _i, mmb->eyecatcher, mmb->hostname, &(mmb->uuid), mmb->kafka_offset);	\
})

static void serialize_tlv_JSON(struct nvmeibt_Str *JSON_output, char *tlv_name, struct nvmeibt_wire_type_len_value *tlv)
{
	if (JSON_output) {
		nvmeibt_Str_sprintf(JSON_output,
						",\n\"tlv_%s\":{\"type\":%d, \"len\":%d, \"crc\":%u, \"idx\":%lld, \"seq_no\":%lld},",
						tlv_name, nvmeibt_tlv_get_type(tlv), nvmeibt_tlv_get_len(tlv), (uint32_t)nvmeibt_tlv_get_CRC(tlv), nvmeibt_tlv_get_idx(tlv), nvmeibt_tlv_get_seq_no(tlv));
	}
}

void serialize_raft_member_json(struct nvmeibt_Str *JSON_output, int i, struct mm_raft_member_conf *member_conf)
{
	struct nvmeibt_urn_uuid		urn_uuid;
	if (!JSON_output) {
		goto out;
	}
	if (i == 0) {
		nvmeibt_Str_sprintf(JSON_output, "\n\"raft_members\":[");
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(member_conf->uuid));
	nvmeibt_Str_sprintf(JSON_output,
						"\n\t{\"n\":\"%d\", \"eyecatcher\":\"%.4s\", \"kafka_offset\":%lld, \"hostname\":\"%s\", \"uuid\":\"%s\"}",
						i, member_conf->eyecatcher, member_conf->kafka_offset, nvmeibt_escape_special_characters(member_conf->hostname).s, urn_uuid.str);
out:;
}

void nvmeibt_raft_align_members_with_committed_wire_buf(struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_raft_member			*member;
	struct mm_raft_member_conf			member_conf;
	int									i;
	struct all_members_wire_buf_ctx		*members_wire_buf;
	int									members_wire_buf_len;
	int64_t								kafka_offset = nvmeibt_offset_and_idx_uninitialized;
	static int							config_tag = 11111;
	int									n_members_in_wire_buf;
	int64_t								new_seq_num;

	// The follower does not make any use of the raft_members. We do build it here
	// The members_list lifecycle is unique, since we first apply (as a leader), add/del a single members, and then send to commit_by_majority
	// Only the leader uses it, so a candidate can start from the committed list (will become a leader, only if this list is respected by the majority)
	// The only issue is that the first raft_member needs to first add itself to the members list (before being elected as leader)
	// As always we start with the list from_persist. Make it the committed.
	// As always, if we receive a list from the leader, make it the committed
	// - If received a members_list from myself (the leader), it already contains the added/deleted member, so I can either ignore or use it
	NFIN;
	if (nvmeibt_raft_is_leader()) {
		// Already aligned, and might contain a new member that is not yet committed (not really, since it sends the full list to itself)
		goto out;
	}
	if (!(my_raft_global.follower_to_commit_persist_and_wire_buf_full)) {
		N_Tf(cbsaytq, "follower_to_commit_persist_and_wire_buf_full=NULL");
		goto  out;
	}
	kafka_offset = nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx));
	members_wire_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(my_raft_global.follower_to_commit_persist_and_wire_buf_full, TLV_TYPE_RAFT_MEMBERS_COMPLETE, (char **)&members_wire_buf);
	if (members_wire_buf_len == 0) {
		N_Tf(cvasy3m, "members_wire_buf_len=0");
		goto out;
	}
	n_members_in_wire_buf = LE_SWAP32(members_wire_buf->n_raft_members);
	NTOMA_ASSERT(tvashj2, members_wire_buf_len == (int)(n_members_in_wire_buf * sizeof(struct mm_raft_member_conf) + sizeof(struct all_members_wire_buf_ctx) + sizeof(EYECATCHER_CNF_END)),
				 "members_wire_buf_len=@INT n_members_in_wire_buf=@INT sizeof(struct mm_raft_member_conf)=@SIZEOF sizeof(struct all_members_wire_buf_ctx)=@SIZEOF total_len=@INT @KAFKA_OFST",
				 members_wire_buf_len, n_members_in_wire_buf, sizeof(struct mm_raft_member_conf), sizeof(struct all_members_wire_buf_ctx),
				 persist_and_wire_buf_get_total_len(my_raft_global.follower_to_commit_persist_and_wire_buf_full), kafka_offset);
	new_seq_num = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed);
	if (my_raft_global.applied_raft_members_seq_no == new_seq_num) {
		N_Tf(rvsj92j, "Same members as before (seq_no=@INT64_TD). No need to add/del members", new_seq_num);
		goto out;
	}
	// A rare case. Add/update all members from the members_wire_buf, and delete the missing ones
	config_tag ++;
	N_Tf(iwjtgpc, "buf_len=@INT n_wire_members=@INT", members_wire_buf_len, n_members_in_wire_buf);
	for (i = 0; i < n_members_in_wire_buf; i++) {
		nvmeibt_raft_member_conf_convert_le_be(&member_conf, &(members_wire_buf->members[i]));
		DUMP_RAFT_MEMBER_CONF(cbjha2, i, &member_conf);
		serialize_raft_member_json(JSON_output, i, &member_conf);
		serialize_end_of_array_obj_to_JSON(i, n_members_in_wire_buf, 0, JSON_output);
		nvmeibt_raft_add_member(member_conf.hostname, i, &(member_conf.uuid), 0, kafka_offset, config_tag);
	}
	// Remove the members that were not in the members_wire_buf
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		if (member->config_tag != config_tag) {
			nvmeibt_raft_del_member(nvmeibt_raft_member_name(member), i--, nvmeibt_raft_member_id(member), 0, kafka_offset);
		}
	}
	my_raft_global.applied_raft_members_seq_no = new_seq_num;
	//
out:
	NFOUT;
}

/*********    Generic   *************/

struct nvmeibt_raft_member *nvmeibt_raft_get_member_by_id(const union nvmeib_uuid *id)
{
	struct nvmeibt_raft_member	*member = nvmeib_hash_search_uuid(my_raft_global.raft_members_hash_by_uuid, id);
	if (!member) {
		N_Tf(vnekfyx, "Member not found id='@UUID_LE'", id);
	}
	return member;
}

static BOOL is_req_vote_rep_or_append_entries_rep(enum nvmeibt_raft_msg_type msg_type)
{
	return !!(msg_type & (RAFT_MSG_APPEND_ENTRIES_REP | RAFT_MSG_REQ_VOTE_REP));
}

static BOOL is_raft_leader_msg(enum nvmeibt_raft_msg_type msg_type)
{
	return !!(msg_type & (RAFT_MSG_APPEND_ENTRIES));
}

BOOL nvmeibt_raft_is_raft_shutdownable_now(void)
{
	if (!raft_do_we_have_a_leader())       // If no leader, then we can shutdown
		return true;
	if (my_raft_global.leader_shutdown_term > 0) {
		if (my_raft_global.role != RAFT_ROLE_LEADER)	// if the leader knows about the shutdown then we can shutdown
			return true;
		if (my_raft_global.n_raft_members <= 1)    // special case, one-node cluster
			return true;
	}

	return false;
}

void nvmeibt_raft_upd_shutdown_term(unsigned long long shutdown_term)
{
	if (shutdown_term > nvmeibt_raft_get_current_term()) {
		my_raft_global.shutdown_term = shutdown_term;
		N_Tf(trace_raft_nvmeibt_raft_upd_shutdown_term, "shutdown_term=@RAFT_TERM", nvmeibt_raft_get_current_term());
	}
}

BOOL nvmeibt_raft_is_shutdown_triggered(void)
{
	int		is_shutdown_triggered = (my_raft_global.shutdown_term && (my_raft_global.shutdown_term >= my_raft_global.first_append_entries_term));
	if (is_shutdown_triggered) {
		N_Tf(trace_raft_nvmeibt_raft_is_shutdown_triggered, "shutdown triggered");
	}
	return is_shutdown_triggered;
}

BOOL nvmeibt_raft_is_leader(void)
{
	return (my_raft_global.role == RAFT_ROLE_LEADER);
}

BOOL nvmeibt_raft_is_raft_valid(void)
{
	int							is_valid;

	is_valid = raft_do_we_have_a_leader();
	if (!is_valid) {
		N_Tf(7hsjkl0, "leader=@LEADER_STR applied_LOG=@INT64_TX (TOPO, follower_committed)=@INT64_TX", raft_get_leader_node_name(),
			 RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed));
    }
	return is_valid;
}

static void verify_persistency_toma_version(int32_t persistent_toma_software_version) {
	if (persistent_toma_software_version != TOMA_SW_COMPATIBILITY_VER)
		N_Wf(trace_raft_verify_persistency_toma_version, "Software version mismatch, '@X'!='@X'", persistent_toma_software_version, TOMA_SW_COMPATIBILITY_VER);
}

static enum nvmeibt_add_rv raft_apply_raft_protocol_params_that_were_read_from_persistence(struct nvmeibt_Str *JSON_output)
{
	enum nvmeibt_add_rv							rv = NVMEIBT_ADD_FAILED;
	unsigned long long							persistent_current_term;
	unsigned long long							persistent_last_rx_append_entries_term;
	int64_t										persistent_kafka_mgmt_zone_number;
	union nvmeib_uuid							persistent_mgmt_DB_uuid;
	int32_t										persistent_toma_software_version;
	int64_t										persistent_raft_calculated_append_entries_rep_time_ns;
	int64_t										persistent_raft_calculated_topo_calc_time_ns;
	uint32_t									persistent_raft_ctx_crc;
	uint32_t									persistent_guaranteed_sw_ver;
	struct timespec								ts;
	struct nvmeibt_topology						*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_persist_and_wire_buf			*buf = my_raft_global.follower_to_commit_persist_and_wire_buf_full;
	union nvmeib_uuid							persistent_voted_for_raft_member_uuid;

	NFIN;
	// Read & convert
	persistent_toma_software_version = LE_SWAP32(buf->buf_sw_ver);
	persistent_current_term = persist_and_wire_buf_get_current_raft_TERM(buf);
	persistent_kafka_mgmt_zone_number = persist_and_wire_buf_get_raft_kafka_mgmt_zone_number(buf);
	persistent_last_rx_append_entries_term = persist_and_wire_buf_get_last_rx_append_entries_raft_TERM(buf);
	persistent_voted_for_raft_member_uuid = persist_and_wire_buf_get_raft_voted_for_uuid(buf);
	persistent_mgmt_DB_uuid = persist_and_wire_buf_get_raft_mgmt_DB_uuid(buf);
	persistent_raft_calculated_append_entries_rep_time_ns = persist_and_wire_buf_get_raft_calculated_leader_append_entries_rep_time_ns(buf);
	persistent_raft_calculated_topo_calc_time_ns = persist_and_wire_buf_get_raft_calculated_leader_topo_calc_time_ns(buf);
	persistent_raft_ctx_crc = persist_and_wire_buf_get_raft_ctx_crc(buf);
	persistent_guaranteed_sw_ver = persist_and_wire_buf_get_raft_guaranteed_sw_ver(buf);
	//
	if (JSON_output) {
		struct nvmeibt_urn_uuid		voted_for_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&persistent_voted_for_raft_member_uuid);
		struct nvmeibt_urn_uuid		mgmt_db_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&persistent_mgmt_DB_uuid);
		nvmeibt_Str_sprintf(JSON_output, "\"buf_sw_ver\":%d,\n", persistent_toma_software_version);
		nvmeibt_Str_sprintf(JSON_output, "\"raft_ctx\":{\"current_term\":%llu, \"last_rx_append_entries_term\":%llu, \"kafka_mgmt_zone_number\":%lld, "
							"\"voted_for_raft_member_uuid\":\"%s\", \"mgmt_DB_uuid\":\"%s\", \"guaranteed_sw_ver\":%u, "
							"\"calculated_append_entries_rep_time_ns\":%lld, \"calculated_topo_calc_time_ns\":%lld, \"raft_ctx_crc\":%u}",
							persistent_current_term, persistent_last_rx_append_entries_term, persistent_kafka_mgmt_zone_number,
							voted_for_urn_uuid.str, mgmt_db_urn_uuid.str, persistent_guaranteed_sw_ver,
							persistent_raft_calculated_append_entries_rep_time_ns, persistent_raft_calculated_topo_calc_time_ns, persistent_raft_ctx_crc);
	}
	//
	N_Tf(tbsjh3k, "Reading: toma_software_version=@SOFTWARE_VERSION current_term=@RAFT_TERM last_rx_append_entries_term=@RAFT_TERM "
		 "kafka_mgmt_zone_number=@INT64_TD voted_for_raft_member_id=@UUID_LE mgmt_DB_uuid=@UUID_LE "
		 "calculated_append_entries_rep_time_ns=@INT64_TD calculated_topo_calc_time_ns=@INT64_TD guaranteed_sw_ver=@SOFTWARE_VERSION crc=@X",
		 persistent_toma_software_version, persistent_current_term, persistent_last_rx_append_entries_term,
		 persistent_kafka_mgmt_zone_number, &persistent_voted_for_raft_member_uuid, &persistent_mgmt_DB_uuid,
		 persistent_raft_calculated_append_entries_rep_time_ns, persistent_raft_calculated_topo_calc_time_ns, persistent_guaranteed_sw_ver, persistent_raft_ctx_crc);
	nvmeibt_global_validate_and_upd_mgmt_DB_uuid(&persistent_mgmt_DB_uuid);
	verify_persistency_toma_version(persistent_toma_software_version);
	cur_topo->persistent_toma_software_version = persistent_toma_software_version;
	my_raft_global.current_term = persistent_current_term;
	nvmeibt_kafka_new_kafka_mgmt_zone_number_received(persistent_kafka_mgmt_zone_number);
	my_raft_global.last_rx_append_entries_term = persistent_last_rx_append_entries_term;
	getnstimeofday_real(&ts);
	cur_topo->running_local_serialization_version = ((unsigned long long)ts.tv_sec << 32) + ts.tv_nsec;
	cur_topo->known_to_leader_local_serialization_version = NVMEIBT_NOT_INITIALIZED_SER_VER;
	nvmeibt_raft_set_voted_for_uuid(&persistent_voted_for_raft_member_uuid);
	nvmeibt_raft_set_guaranteed_sw_ver(persistent_guaranteed_sw_ver);

	rv = NVMEIBT_ADD_NEW;
	NFOUT;
	return rv;
}

static bool is_tlv_crc_ok(struct nvmeibt_wire_type_len_value *type_len_ptr, char *buf, int len, char *type_str)
{
	uint32_t					msg_crc, calc_crc;
	bool						rv = 1;

	msg_crc = type_len_ptr->tlv_crc;
	type_len_ptr->tlv_crc = 0;
	calc_crc = crc32(0, type_len_ptr, sizeof(*type_len_ptr));
	calc_crc = crc32(calc_crc, buf, len);
	type_len_ptr->tlv_crc = msg_crc;
	msg_crc = LE_SWAP32(msg_crc);
	if (calc_crc != msg_crc) {
		N_Ef(kk111s9, "@STR tlv crc error. received=@X, calc=@X", type_str, msg_crc, calc_crc);
		rv = 0;
	}
	return rv;
}

static bool is_persist_and_wire_buf_crc_and_len_ok(struct nvmeibt_persist_and_wire_buf *buf, int data_len)
{
	int							section_len;
	char						*section_buf;
	uint32_t					msg_crc, calc_crc;
	bool						rv = 1;

	section_len = nvmeibt_tlv_get_len(&(buf->topo_ctx)) +
				  nvmeibt_tlv_get_len(&(buf->topo_config_ctx)) +
				  nvmeibt_tlv_get_len(&(buf->kafka_mgmt_config_ctx)) +
				  nvmeibt_tlv_get_len(&(buf->raft_members_ctx));
	if (section_len != data_len) {
		N_Ef(hwuroma, "sections_len=@INT != received_len=@INT", section_len, data_len);
		rv = 0;
	}

	msg_crc = persist_and_wire_buf_get_raft_ctx_crc(buf);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, 1);
	calc_crc = persist_and_wire_buf_get_raft_ctx_crc(buf);
	if (calc_crc != msg_crc) {
		N_Ef(kk391s9, "raft crc error. received=@X, calc=@X", msg_crc, calc_crc);
		rv = 0;
	}

	section_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(buf, TLV_TYPE_TOPO_COMPLETE, &section_buf);
	if (!is_tlv_crc_ok(&buf->topo_ctx, section_buf, section_len, "TOPO"))
		rv = 0;
	section_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(buf, TLV_TYPE_TOPO_CONFIG_COMPLETE, &section_buf);
	if (!is_tlv_crc_ok(&buf->topo_config_ctx, section_buf, section_len, "TOPO_CONFIG"))
		rv = 0;
	section_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(buf, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, &section_buf);
	if (!is_tlv_crc_ok(&buf->kafka_mgmt_config_ctx, section_buf, section_len, "MGMT_CONFIG"))
		rv = 0;
	section_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(buf, TLV_TYPE_RAFT_MEMBERS_COMPLETE, &section_buf);
	if (!is_tlv_crc_ok(&buf->raft_members_ctx, section_buf, section_len, "RAFT_MEMBERS"))
		rv = 0;
	return rv;
}

int nvmeibt_raft_read_persistence_and_upd_committed(const char *persistence_file_name, struct nvmeibt_Str *JSON_output)
{
	int 						rv = -1;
	int		 					fd = -1;
	int							is_cache_dir_writeable;
	int							is_toma_persistency_file_exist;
	int							is_toma_persistency_file_writeable;
	struct nvmeibt_Str			*persistency_ctx = NULL;
	char						*section_buf;
	int							section_buf_len;
	size_t						persistency_ctx_len;

	NFIN;
	is_cache_dir_writeable = !access(NVMEIBT_PERSISTENCY_CACHE_DIR, W_OK);
	is_toma_persistency_file_exist = !access(persistence_file_name, F_OK);
	is_toma_persistency_file_writeable = !access(persistence_file_name, W_OK);
	// Verify that we are good with the file permissions
	// Read_raft_persistency
	if ((is_toma_persistency_file_exist && !is_toma_persistency_file_writeable) ||
		(!is_toma_persistency_file_exist && !is_cache_dir_writeable)) {
		N_ETf(fju8576, "No permission to write on file @TOMA_PERSISTENCY_FILE_NAME, @AUTO_ERRNO", persistence_file_name);
		goto out;
	}
	persistency_ctx = NNVMEIBT_STR_ALLOC(hki9t67);
	if (is_toma_persistency_file_exist) {
		// Read the persistency file
		fd = NNVMEIBT_OPEN_READ(flori85, persistence_file_name, 1);
		if (fd < 0) {
			N_ETf(gki9845, "Error while opening the file @TOMA_PERSISTENCY_FILE_NAME. @AUTO_ERRNO", persistence_file_name);
			goto out;
		}
		NNVMEIBT_STR_FREAD(fki9e56, persistency_ctx, fd);
		persistency_ctx_len = nvmeibt_Str_strlen(persistency_ctx);
		if (persistency_ctx_len > 0 && persistency_ctx_len < sizeof(struct nvmeibt_persist_and_wire_buf)) {
			N_Ef(dlo09cx, "Unreasonable topology persistence_ctx_len=@SIZE_T", nvmeibt_Str_strlen(persistency_ctx));
			goto out;
		}
		if (!is_persist_and_wire_buf_crc_and_len_ok((struct nvmeibt_persist_and_wire_buf *)nvmeibt_Str_str(persistency_ctx),
													persistency_ctx_len - (int)sizeof(struct nvmeibt_persist_and_wire_buf)))
			goto out;
	} else {
		persistency_ctx_len = nvmeibt_Str_strlen(persistency_ctx);
		N_Tf(skiu386, "No previous raft file @TOMA_PERSISTENCY_FILE_NAME", persistence_file_name);
	}
	//
	N_Tf(dlo09r5, "Persistency length = @SIZE_T", persistency_ctx_len);
	TODO(Stop using struct nvmeibt_Str for the persistence, as it is binary, and it works only since those functions of nvmeibt_Str do not care about a '\0');
	// Necessary init
	my_raft_global.follower_to_commit_persist_and_wire_buf_full = alloc_persist_and_wire_buf(max(persistency_ctx_len, sizeof(*(my_raft_global.follower_to_commit_persist_and_wire_buf_full))));
	my_raft_global.follower_to_leader_wire_buf = alloc_persist_and_wire_buf(max(persistency_ctx_len, sizeof(*(my_raft_global.follower_to_leader_wire_buf))));
	//
	if (!is_toma_persistency_file_exist) {
		rv = 0;
		goto out;
	}
	//
	memcpy(my_raft_global.follower_to_commit_persist_and_wire_buf_full, nvmeibt_Str_str(persistency_ctx), persistency_ctx_len);
	memcpy(my_raft_global.follower_to_leader_wire_buf, nvmeibt_Str_str(persistency_ctx), persistency_ctx_len);
	persist_and_wire_buf_validate_len(my_raft_global.follower_to_commit_persist_and_wire_buf_full);
	//
	raft_apply_raft_protocol_params_that_were_read_from_persistence(JSON_output);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(5gbduw9, TOPO,              follower_committed, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(dfkxmi3, TOPO,              follower_submitted, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(0sm4kou, TOPO_CONFIG,       follower_committed, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_config_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(v2tiaoq, TOPO_CONFIG,       follower_submitted, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_config_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vxta7y2, KAFKA_MGMT_CONFIG, follower_committed, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->kafka_mgmt_config_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(xcat62h, KAFKA_MGMT_CONFIG, follower_submitted, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->kafka_mgmt_config_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vshrk34, RAFT_MEMBERS,      follower_committed, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cs7834h, RAFT_MEMBERS,      follower_submitted, nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cs7834h, RAFT_MEMBERS,      leader_calculated,  nvmeibt_tlv_get_idx(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));	// Avoid received-affected & not committed
	//
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbye8k3, RAFT_MEMBERS_SEQ_NO, follower_committed, nvmeibt_tlv_get_seq_no(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vnsi0o3, RAFT_MEMBERS_SEQ_NO, follower_submitted, nvmeibt_tlv_get_seq_no(&(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));
	nvmeibt_kafka_set_last_sent_to_toma_targets_updates_seq_no(RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
	// raft_members
	serialize_tlv_JSON(JSON_output, "raft_members", &(my_raft_global.follower_to_commit_persist_and_wire_buf_full->raft_members_ctx));
	nvmeibt_raft_align_members_with_committed_wire_buf(JSON_output);
	//
	// The stored KAFKA_MGMT_CONFIG is used only if I become a leader
	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(my_raft_global.follower_to_commit_persist_and_wire_buf_full, TLV_TYPE_KAFKA_MGMT_CONFIG_COMPLETE, &section_buf);
	serialize_tlv_JSON(JSON_output, "KAFKA_MGMT_CONFIG_FULL", &(my_raft_global.follower_to_commit_persist_and_wire_buf_full->kafka_mgmt_config_ctx));
	if (section_buf_len) {
		if (nvmeibt_parse_buf(section_buf, section_buf_len, 1, NVMEIBT_NOT_INITIALIZED_SER_VER, NULL, NVMEIBT_CSV_TYPE_FULL_KAFKA_MGMT_CONFIG_VOLUMES,
							  JSON_output) < 0) {
			goto out;
		}
		{	// The FOLLOWER_KAFKA_OFFSET_APPLIED() is affected by the parsing
			const int64_t parsed_val = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_applied);
			SET_RAFT_COMMIT_LIFECYCLE_VAL(br754k3, KAFKA_MGMT_CONFIG, follower_committed, parsed_val);
			SET_RAFT_COMMIT_LIFECYCLE_VAL(b3454k3, KAFKA_MGMT_CONFIG, follower_submitted, parsed_val);
			SET_RAFT_COMMIT_LIFECYCLE_VAL(34754k3, KAFKA_MGMT_CONFIG, follower_to_apply,  parsed_val);
		}
	} else {
		N_Wf(hsiqok3, "section_buf_len=0 tlv_KAFKA_MGMT_CONFIG_FULL");
		if (JSON_output) nvmeibt_Str_strcat(JSON_output, "\"KAFKA_MGMT_CONFIG_FULL\" : {}");
	}
	serialize_tlv_JSON(JSON_output, "FULL_TOPO_CONFIG_VOLUMES", &(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_config_ctx));
	if (nvmeibt_topology_parse_a_config(NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES, JSON_output) < 0) {
		goto out;
	}
	if ((JSON_output) && (nvmeibt_Str_end(JSON_output)[-1] == ',')) {	// Missing FULL_TOPO_CONFIG object finishing with ','  Todo: Fix me properly in the above func
		nvmeibt_Str_strcat(JSON_output, "\"FULL_TOPO_CONFIG\" : {}");
	}
	section_buf_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(my_raft_global.follower_to_commit_persist_and_wire_buf_full, TLV_TYPE_TOPO_COMPLETE, &section_buf);
	serialize_tlv_JSON(JSON_output, "TOPO_FULL", &(my_raft_global.follower_to_commit_persist_and_wire_buf_full->topo_ctx));
	if (section_buf_len) {
		if (nvmeibt_parse_buf(section_buf, section_buf_len, 0, NVMEIBT_NOT_INITIALIZED_SER_VER, NULL, NVMEIBT_CSV_TYPE_TOPO,
							  JSON_output) < 0) {
			goto out;
		}
		if (JSON_output && nvmeibt_Str_end(JSON_output)[-1] == ']')	// Missing praids array finishing with '}'  Todo: Fix me properly in the above func
			nvmeibt_Str_strcat(JSON_output, "}");
	} else {
		N_Wf(bfgiker, "section_buf_len=0 tlv_FULL_TOPO_CONFIG_VOLUMES");
		if (JSON_output) nvmeibt_Str_strcat(JSON_output, "\"FULL_TOPO\" : {}");
	}
	{ // raft_TERM
		const int64_t parsed_val = persist_and_wire_buf_get_current_raft_TERM(my_raft_global.follower_to_commit_persist_and_wire_buf_full);
		SET_RAFT_COMMIT_LIFECYCLE_VAL(zyb2hsu, current_raft_TERM, follower_committed, parsed_val);
		SET_RAFT_COMMIT_LIFECYCLE_VAL(7dbn6k3, current_raft_TERM, follower_submitted, parsed_val);
	}
	rv = 0;
out:
	raft_convert_to_follower(nvmeibt_raft_get_voted_for_uuid(), raft_get_leader_node_uuid());	// We just read it from persistence
	NNVMEIBT_CLOSE(alo09r5, fd);
	NNVMEIBT_STR_FREE(floi4e3, persistency_ctx);
	NFOUT;
	return rv;
}

struct leader_name_wq_entry {
	struct nvmeibt_wq_entry		wq_entry;
	char						leader_node_name[NVMEIB_HOST_NAME_LEN];
};

static void write_leader_name_to_file(const char *node_name)
{
	int 					fd = -1;
	const char				file_name[] = TOMA_LOG_DIR"/toma_leader_name";
	char					leader_str[NVMEIB_HOST_NAME_LEN+4];

	NFIN;
	if (nvmeibt_toma_is_running_as_a_utility()) {
		goto out;
	}
	fd = NNVMEIBT_OPEN(trace_raft_write_leader_name_to_file, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0755);
	if (fd < 0) {
		N_ETf(error_raft_write_leader_name_to_file, "Error while opening the file @FILE_NAME for writing @AUTO_ERRNO", file_name);
		goto out;
	} else {
		int written = snprintf(leader_str, NVMEIB_HOST_NAME_LEN, "%s\n", node_name);
		if (NNVMEIBT_PWRITE(warn_raft_write_leader_name_to_file, fd, leader_str, written, 0, 0) < 0)
			goto out;
	}
out:
	NNVMEIBT_CLOSE(trace_1_raft_write_leader_name_to_file, fd);
	NFOUT;
}

static void write_leader_name_to_file_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct leader_name_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct leader_name_wq_entry, wq_entry);

	N_Tf(trace_raft_write_leader_name_to_file_wrapper, "Writing leader name=@NAME to file",  entry->leader_node_name);
	write_leader_name_to_file(entry->leader_node_name);

	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);

	NFOUT;
}

static void write_leader_name_to_file_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct leader_name_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct leader_name_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_raft_write_leader_name_to_file_freer, entry);
	NFOUT;
}

/*
 * Rename 8->9, 7->8 ... 1->2
 * Link 1 to point to 0 (in addition to 0)
 */
static int keep_old_versions_of_toma_persistency_file(void)
{
	int		rv = -1;
	char	old_name[strlen(toma_persistency_file_name) + 1];
	char	new_name[strlen(toma_persistency_file_name) + 1];
	char	name_1[strlen(toma_persistency_file_name) + 1];
	int		i;
	const int	n_copies = 4;

	NFIN;
	nvmeibt_strlcpy(name_1, toma_persistency_file_name, sizeof(name_1));
	name_1[strlen(toma_persistency_file_name) - 1] = '1';
	if (access(name_1, F_OK) < 0) {
		N_Tf(trace_raft_keep_old_versions_of_toma_persistency_file, "File @NAME_1 does not exist. Skipping rename of old files", name_1);
		goto skipped_rename;
	}

	nvmeibt_strlcpy(old_name, toma_persistency_file_name, sizeof(old_name));
	nvmeibt_strlcpy(new_name, toma_persistency_file_name, sizeof(new_name));
	new_name[strlen(toma_persistency_file_name) - 1] = ('0' + n_copies);
	NNVMEIBT_UNLINK(warn_raft_keep_old_versions_of_toma_persistency_file, new_name, 1);

	// Since this takes time, keeping n_copies
	for (i = n_copies - 1; i >= 1; --i) {
		old_name[strlen(toma_persistency_file_name) - 1] = ('0' + i);
		if (access(old_name, F_OK) < 0) {
			continue;	// File does not exist, skip the rename
		}

		new_name[strlen(toma_persistency_file_name) - 1] = ('0' + i + 1);
		if (NNVMEIBT_RENAME(warn_1_raft_keep_old_versions_of_toma_persistency_file, old_name, new_name) < 0) {
			N_Ef(error_raft_keep_old_versions_of_toma_persistency_file, "Error rename(@OLD_NAME ,@NEW_NAME). @AUTO_ERRNO", old_name, new_name);
			goto out;
		}
	}
skipped_rename:
	// Make name_1 also link to toma_persistency_file_name
	if (NNVMEIBT_LINK(warn_2_raft_keep_old_versions_of_toma_persistency_file, toma_persistency_file_name, name_1) < 0) {
		N_Ef(error_1_raft_keep_old_versions_of_toma_persistency_file, "Error link(@TOMA_PERSISTENCY_FILE_NAME, @NAME_1) @AUTO_ERRNO", toma_persistency_file_name, name_1);
		goto out;
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

static int write_new_version_of_toma_persistency_file(struct nvmeibt_Buf *buf)
{
	int 						rv = -1;
	int 						topo_file_fd = -1;
	int			 				dir_fd = -1;
	char						interim_name[strlen(toma_persistency_file_name) + 1];
	char						dir_name[strlen(toma_persistency_file_name) + 1];
	char						*last_slash;

	NFIN;

	// Prepare the dir_name
	nvmeibt_strlcpy(dir_name, toma_persistency_file_name, sizeof(dir_name));
	last_slash = strrchr(dir_name, '/');
	if (!last_slash) {
		N_Ef(error_raft_write_new_version_of_toma_persistency_file, "No '/' in @DIR_NAME", dir_name);
		goto out;
	}
	*(last_slash + 1) = '\0';

	nvmeibt_strlcpy(interim_name, toma_persistency_file_name, sizeof(interim_name));
	interim_name[strlen(toma_persistency_file_name) - 1] = 'T';

	// Open the file and write
	topo_file_fd = NNVMEIBT_OPEN(trace_raft_write_new_version_of_toma_persistency_file, interim_name, O_CREAT | O_WRONLY | O_TRUNC, 0755);
	if (topo_file_fd < 0) {
		N_Ef(error_1_raft_write_new_version_of_toma_persistency_file, "Error while opening the file @INTERIM_NAME for writing @AUTO_ERRNO", interim_name);
		goto out;
	}

	if (NNVMEIBT_BUF_FWRITE(tq23mx0, buf, topo_file_fd) < 0) {
		N_Ef(error_2_raft_write_new_version_of_toma_persistency_file, "write failed: @AUTO_ERRNO");
		goto out;
	}

	// Sync file and close
	if (NNVMEIBT_FSYNC(warn_1_raft_write_new_version_of_toma_persistency_file, topo_file_fd) < 0) {
		N_Tf(trace_1_raft_write_new_version_of_toma_persistency_file, "fsync failed: @AUTO_ERRNO");
		goto out;
	}

	// Rename from interim to actual
	if (NNVMEIBT_RENAME(warn_2_raft_write_new_version_of_toma_persistency_file, interim_name, toma_persistency_file_name) < 0) {
		N_Ef(error_3_raft_write_new_version_of_toma_persistency_file, "Error rename(@INTERIM_NAME ,@TOMA_PERSISTENCY_FILE_NAME) @AUTO_ERRNO", interim_name, toma_persistency_file_name);
		goto out;
	}

	// Sync the directory
	dir_fd = NNVMEIBT_OPEN(trace_2_raft_write_new_version_of_toma_persistency_file, dir_name, O_DIRECTORY);	// Open from scratch per cycle
	if (dir_fd < 0) {
		N_ETf(error_4_raft_write_new_version_of_toma_persistency_file, "Error while opening dir @DIR_NAME for writing @AUTO_ERRNO", dir_name);
		goto out;
	}
	if (NNVMEIBT_FSYNC(warn_3_raft_write_new_version_of_toma_persistency_file, dir_fd) < 0) {
		N_Tf(trace_3_raft_write_new_version_of_toma_persistency_file, "Failed fsync(dir) failed: @AUTO_ERRNO");
		goto out;
	}

	rv = 0;
out:
	NNVMEIBT_CLOSE(trace_4_raft_write_new_version_of_toma_persistency_file, topo_file_fd);
	NNVMEIBT_CLOSE(trace_5_raft_write_new_version_of_toma_persistency_file, dir_fd);

	NFOUT;
	return rv;
}

void nvmeibt_raft_fill_persistence_buf_for_system_disk(struct nvmeibt_Buf *buf_to_save)
{
	int		total_len;

	NFIN;
	total_len = persist_and_wire_buf_get_total_len(my_raft_global.follower_to_commit_persist_and_wire_buf_full);
	NNVMEIBT_BUF_RESIZE(rbqau5j, buf_to_save, (size_t)total_len);
	memcpy(((char *)(buf_to_save->data_buf)), my_raft_global.follower_to_commit_persist_and_wire_buf_full, total_len);
	NFOUT;
}

int nvmeibt_raft_save_toma_state_to_persistency(struct nvmeibt_persistency_wq_entry *entry)
{
	int 									rv = -1;
	struct nvmeibt_persist_and_wire_buf		*follower_persist_buf;
	size_t									follower_persist_buf_len;

	NFIN;
	follower_persist_buf = (struct nvmeibt_persist_and_wire_buf *)(entry->follower_persist_buf_full.data_buf);
	follower_persist_buf_len = entry->follower_persist_buf_full.buf_len;
	if (follower_persist_buf_len < 50) {
		N_Ef(fji87r9, "Too short follower_persist_buf_full=@PTR len=@LEN_LONG", &(entry->follower_persist_buf_full), follower_persist_buf_len);
		goto out;
	}
	persist_and_wire_buf_validate_len(follower_persist_buf);
	if (!is_persist_and_wire_buf_crc_and_len_ok(follower_persist_buf, persist_and_wire_buf_get_total_len(follower_persist_buf) - sizeof(*follower_persist_buf))) {
		goto out;
	}

	if (write_new_version_of_toma_persistency_file(&(entry->follower_persist_buf_full)) < 0) {
		N_WTf(aji985r, "Failed to write current version of toma persistency");
		goto out;
	}

	if (keep_old_versions_of_toma_persistency_file() < 0) {
		N_Wf(djur873, "Failed to keep old versions of toma persistency");
		goto out;
	}

	/* if replay: skip (only do leader's logic and topology calculation) */
	if (nvmeibt_replay_is_enabled())
		goto skip_write_metadata;

	rv = 0;

	// Finished updating on-disk metadata of disks/segments
	goto out;

skip_write_metadata:
	rv = 0;

out:
	NFOUT;
	return rv;
}

BOOL nvmeibt_raft_is_same_msg(void *msg1, void *msg2)
{
	return !memcmp((struct raft_msg *)msg1, (struct raft_msg *)msg2, sizeof(struct raft_msg));
}

static void raft_send_topo_cb(__attribute__((__unused__)) void *arg, int status)
{
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	struct timespec				now;
	long long int				diff_timeout_nsec;

	const int cnt = atomic_dec_return(&cur_topo->in_transmission_cnt);
	N_Tf(t_bb_1, "End of topo tx status=@INT, tx_remained=@INT", status, cnt);
	if (cnt == 0) {
		getnstimeofday_boot(&now);
		diff_timeout_nsec = timespec_diff_ns(now, cur_topo->last_raft_distribution_timestamp);
		if (diff_timeout_nsec < raft_leader_heartbeat_timeout_nsec)
			N_Tf(dii022z,"Raft distribution time=@INT64 nsec", diff_timeout_nsec);
		else
			N_Wf(dii022a,"Raft distribution time=@INT64 nsec", diff_timeout_nsec);
	}
}

static void raft_send_topo_reply_cb(__attribute__((__unused__)) void *arg, int status)
{
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	const int cnt = atomic_dec_return(&cur_topo->in_transmission_rep_cnt);
	N_Tf(t_bb_10, "End of topo reply tx status=@INT, tx_remained=@INT", status, cnt);
}

#define NRAFT_DUMP_MSG(name, _raft_msg, node, _msg_data_len)		({													\
	struct raft_msg							*m = (_raft_msg);															\
	struct nvmeibt_persist_and_wire_buf		*_b = &(m->persist_and_wire_buf);											\
	persist_and_wire_buf_validate_len(_b);																				\
	N_Tf(name, "current_term=@RAFT_TERM "																				\
		 "commit_IDX<<TOPO=@INT64_TX TOPO_CONFIG=@INT64_TX KAFKA_MGMT_CONFIG_OFFSET=@INT64_TX "							\
		 "RAFT_MEMBERS_OFFSET=@INT64_TX RAFT_MEMBERS_SEQ_NO=@INT64_TX>> "												\
		 "apply_IDX<<TOPO=@INT64_TX TOPO_CONFIG=@INT64_TX RAFT_MEMBERS_OFFSET=@INT64_TX>> "								\
		 " msg_num=@UINT member=@STR msg_type=@MSG_TYPE_STR, "															\
		 "is_vote_granted=@BOOL is_with_raft_log=@IS_WITH_RAFT_LOG "													\
		 "ser_ver=@LLD data_len=@INT",																					\
		 (m)->current_term,																								\
		 (_b ? nvmeibt_tlv_get_idx(&(_b->topo_ctx)) : 0),																\
		 (_b ? nvmeibt_tlv_get_idx(&(_b->topo_config_ctx)) : 0),														\
		 (_b ? nvmeibt_tlv_get_idx(&(_b->kafka_mgmt_config_ctx)) : 0),													\
		 (_b ? nvmeibt_tlv_get_idx(&(_b->raft_members_ctx)) : 0),														\
		 (_b ? nvmeibt_tlv_get_seq_no(&(_b->raft_members_ctx)) : 0),													\
		 (m)->applied_TOPO_idx,	(m)->applied_TOPO_CONFIG_idx, (m)->applied_raft_members_offset,							\
		 (m)->append_entries_msg_num, nvmeibt_node_name(node),															\
		 ((m)->msg_type == RAFT_MSG_REQ_VOTE ? "REQ_VOTE" : (m)->msg_type == RAFT_MSG_REQ_VOTE_REP ? "REQ_VOTE_REP" :	\
		 (m)->msg_type == RAFT_MSG_APPEND_ENTRIES ? "APPEND_ENTRIES" : (m)->msg_type == RAFT_MSG_APPEND_ENTRIES_REP ?	\
		 "APPEND_ENTRIES_REP" : TOMA_ERR_STR),																		\
		 (m)->is_vote_granted, (m)->is_with_raft_log, (m)->local_serialization_version, _msg_data_len);					\
})

static int raft_send_msg_to_peer(
		enum nvmeibt_raft_msg_type			msg_type,
		struct nvmeibt_node					*dst_node,
		int									is_vote_granted,
		int									is_with_raft_log,
		struct nvmeibt_persist_and_wire_buf	*persist_and_wire_buf,
		char								flags,
		int									msg_data_len
		)
{
	int							rv = 0;
	struct raft_msg 		 	*msg = NULL;
	int							total_msg_size;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	if (!dst_node) {
		N_Tf(rkso3la, "dst_node=NULL, probably no member->its_node. Skipping");
		rv = 1;
		goto out;
	}
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Tf(u8ui8nb, "!nvmeibt_topology_is_HW_config_functional()");
		goto out;
	}

	if (msg_type == RAFT_MSG_APPEND_ENTRIES_REP)
		dst_node->append_entries_rep_was_not_sent = false;
	// alloc msg
	total_msg_size = offsetof(typeof(*msg), persist_and_wire_buf) + persist_and_wire_buf_get_total_len(persist_and_wire_buf);
	msg = NNVMEIBT_BM_CALLOC(6gwuyj3, total_msg_size);
	// My_raft state
	msg->software_version = TOMA_SW_COMPATIBILITY_VER;
	nvmeibt_strlcpy(msg->git_commit_id, GIT_COMMIT_ID, sizeof(msg->git_commit_id));
	msg->src_node_id = *raft_get_my_uuid();
	msg->src_node_idx = 0; /*cur_topo->my_node->idx_in_cur_topo;*/

	if (is_raft_leader_msg(msg_type)) {
		msg->append_entries_msg_num = my_raft_global.last_tx_append_entries_msg_num;
		msg->applied_TOPO_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority);
		msg->applied_TOPO_CONFIG_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_committed_by_majority);
		msg->applied_raft_members_offset = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_committed_by_majority);
		//
		msg->local_serialization_version = raft_member_get_last_local_serialization_version(nvmeibt_node_get_raft_member(dst_node));
		msg->current_term = nvmeibt_raft_get_current_term();
	} else {	// Follower's msg
		msg->applied_TOPO_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied);
		msg->applied_TOPO_CONFIG_idx = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied);
		msg->applied_raft_members_offset = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_applied);
		msg->append_entries_msg_num = my_raft_global.last_rx_append_entries_msg_num;
		msg->local_serialization_version = cur_topo->running_local_serialization_version;
		msg->current_term = nvmeibt_raft_get_current_term(); 	// Could also be: msg->current_term = RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_committed);
	}

	msg->shutdown_term = my_raft_global.shutdown_term;
	// Protocol
	msg->msg_type = msg_type;
	msg->flags = flags;
	msg->dst_node_id = *nvmeibt_node_UUID(dst_node);
	msg->dst_node_idx = 0; /*dst_node->idx_in_cur_topo;*/
	msg->is_vote_granted = is_vote_granted;
	msg->is_with_raft_log = is_with_raft_log;
	msg->persist_and_wire_buf = *persist_and_wire_buf;	// Copy the tlvs etc. The data will be copied immediately after only if is_me
	// Data
	msg->msg_data_len = msg_data_len;
	// send the msg
	NRAFT_DUMP_MSG(cvb649o, msg, dst_node, msg_data_len);
	if (nvmeibt_node_is_my_node(dst_node)) {
		// Behave as if I received the message from myself (leader/candidate/follower)
		if (is_with_raft_log) {
			memcpy(msg->persist_and_wire_buf.data, persist_and_wire_buf->data, (persist_and_wire_buf_get_total_len(persist_and_wire_buf) - offsetof(typeof(*persist_and_wire_buf), data)));	// Generate a proper persist_and_wire_buf
		}
		rv = dispatch_raft_msg(msg, dst_node);
	} else if (cur_topo->raft_pause_mode & RAFT_OUT_PAUSED) {
		N_Tf(trace_0_1_raft_send_msg_to_peer, "Not sending message to node @NODE_NAME, RAFT is paused", nvmeibt_node_name(dst_node));
	} else if (nvmeibt_node_get_tx_conn_ctx(dst_node)) {
		struct nvmeibt_msg_request req;
		memset(&req, 0, sizeof(req));		// Default initializer (callbacks are NULL)
		req.msg_type = NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT;
		req.user = SRM_EMPTY_USER;
		req.cnst_msg = msg;
		req.msg_len = sizeof(*msg);
		req.cnst_data = msg_data_len > 0 ? persist_and_wire_buf->data : NULL;
		req.data_len = msg_data_len > 0 ? msg_data_len : 0;
		if (is_with_raft_log) {							// We need a callback of send_msg finish to be able to reuse the buffer for next message
			if (is_raft_leader_msg(msg_type)) {
				const int cnt = atomic_inc_return(&cur_topo->in_transmission_cnt);
				N_Tf(t_bb_2, "Start of topo tx, tx_remained=@INT", cnt);
				req.cbs.send_c = raft_send_topo_cb;
			} else {
				const int cnt = atomic_inc_return(&cur_topo->in_transmission_rep_cnt);
				N_Tf(t_bb_20, "Start of topo reply tx, tx_remained=@INT", cnt);
				req.cbs.send_c = raft_send_topo_reply_cb;
			}
		}
		convert_raft_msg_header_le_be(msg);
		msg->raft_hdr_crc = 0;
		msg->raft_hdr_crc = LE_SWAP32(crc32(0, msg, sizeof(*msg)));
		N_Tf(trace_1_raft_raft_send_msg_to_peer, "Sending message to node @NODE_NAME", nvmeibt_node_name(dst_node));
		rv = nvmeibt_node_send(dst_node, &req);
		if (rv < 0) {
			N_Tf(trace_2_raft_raft_send_msg_to_peer, "Failed to send msg to node @NODE_NAME", nvmeibt_node_name(dst_node));
			if (req.cbs.send_c)
				req.cbs.send_c(req.arg, -EIO);	// Send Callback will not arrive, simulate failure
			goto out;
		}
		if (!is_raft_leader_msg(msg_type)) {
			SET_RAFT_COMMIT_LIFECYCLE_VAL(4is0cbh, RAFT_MEMBERS,        follower_sent_to_leader, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS,        follower_committed));
			SET_RAFT_COMMIT_LIFECYCLE_VAL(bbbxk43, RAFT_MEMBERS_SEQ_NO, follower_sent_to_leader, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed));
		}
	} else
		N_Tf(trace_3_raft_raft_send_msg_to_peer, "No RAFT connection to node @NODE_NAME", nvmeibt_node_name(dst_node));
out:
	NNVMEIBT_BM_FREE(vrhjskr, msg);
	return rv;
}

static BOOL is_applied_topo_ready_and_different(struct nvmeibt_Buf *applied_topo_buf)
{
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	// The applied_topo was generated but unknown to leader yet
	return ((cur_topo->running_local_serialization_version != cur_topo->known_to_leader_local_serialization_version) &&
			(applied_topo_buf->buf_len != 0));
}

static int check_peer_eligibility_for_leader(const struct raft_msg *msg, struct nvmeibt_node *peer_node, bool is_req_vote)
{
	int							eligibility;

	// We get here after is_incoming_msg_valid(), that does much filtering
	if (msg->current_term > nvmeibt_raft_get_current_term()) {
		N_Ef(tcdvsjw, "Impossible, before dispatch we assign a higher msg->current_term to current_term");
		nvmeibt_abort(ES_FATAL);
	}
	if (msg->current_term < nvmeibt_raft_get_current_term()) {
		// If it is an APPEND_ENTRIES from an active leader, and we progressed our raft_term by repeated convert_to_candidate
		// then we send a !is_vote_granted. That leader will take our high raft_term, and soon start new elections, where it will
		// win over us.
		// From the protocol
		N_Tf(7chasyq, "msg->current_term=@LLX < current_term=@LLX", msg->current_term, nvmeibt_raft_get_current_term());
		eligibility = -1;
		goto out;
	}
	if (is_req_vote) {
		if (raft_do_we_have_a_voted_for() && !raft_is_voted_for_uuid(nvmeibt_node_UUID(peer_node))) {
			N_Tf(3iak8sc, "Already voted_for a different uuid @STR", nvmeibt_node_name(nvmeibt_node_get_node_by_id(nvmeibt_raft_get_voted_for_uuid())));
			eligibility = -1;
			// We are stuck with the voted_for_raft_member. Can be recovered when we re-become-follower, e.g., get a higher term
		} else {
			switch (compare_incoming_and_my_persist_and_wire_buf_tlv(&msg->persist_and_wire_buf)) {
			case INCOMING_IDX_IS_LOWER :
				eligibility = -1;
				break;
			case INCOMING_IDX_IS_HIGHER :
				eligibility = 0;
				break;
			case IDXS_ARE_EQUAL :
				eligibility = (msg->software_version < nvmeibt_raft_get_guaranteed_sw_ver()) ? -1 : 0;
				N_Tf(u87ubv4, "incoming_ver=@SOFTWARE_VERSION, guaranteed_ver=@SOFTWARE_VERSION, eligibility=@INT",
					 msg->software_version, nvmeibt_raft_get_guaranteed_sw_ver(), eligibility);
				break;
			default: // Just to shush the stupid compiler
				eligibility = -1;
				N_Ef(u889223, "Impossible, wrong comparison result");
				nvmeibt_abort(ES_FATAL);
			}
		}
		goto out;
	}
	// Received APPEND_ENTRIES
	if (msg->current_term > my_raft_global.last_rx_append_entries_term) {
		// A new leader. Accept
		my_raft_global.last_rx_append_entries_term = msg->current_term;
		eligibility = 0;
	} else if (my_raft_global.last_rx_append_entries_msg_num > msg->append_entries_msg_num) {
		// Same leader, old msg
		N_Wf(bbuy77q, "Old msg received, ignoring. last_num=@UINT > msg_num=@UINT",
			 my_raft_global.last_rx_append_entries_msg_num, msg->append_entries_msg_num);
		eligibility = 1;
	} else {
		eligibility = 0;
	}
out:
	if (eligibility == 0) {	// Accepted
		my_raft_global.last_rx_append_entries_msg_num = msg->append_entries_msg_num;
	}
	return eligibility;
}

static void raft_leader_check_committed_by_majority_and_act_upon(void)
{
	NFIN;
	if ((RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_n_peers_committed) * 2) > my_raft_global.n_raft_active_members) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(tbjxkdo, TOPO,                leader_committed_by_majority, RAFT_COMMIT_LIFECYCLE_VAL(TOPO,                leader_to_commit));
		nvmeibt_topology_leader_mark_all_modified_praids_report_to_mgmt_due_to_committed_by_majority();
	}
	if ((RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_n_peers_committed) * 2) > my_raft_global.n_raft_active_members) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(vkccole, TOPO_CONFIG,         leader_committed_by_majority, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG,         leader_to_commit));
	}
	if ((RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_n_peers_committed) * 2) > my_raft_global.n_raft_active_members) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(lg04vdy, KAFKA_MGMT_CONFIG,   leader_committed_by_majority, RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG,   leader_to_commit));
	}
	if ((RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_n_peers_committed) * 2) > my_raft_global.n_raft_active_members) {
		SET_RAFT_COMMIT_LIFECYCLE_VAL(1vzymei, RAFT_MEMBERS,        leader_committed_by_majority, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS,        leader_to_commit));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(tcsbvsk, RAFT_MEMBERS_SEQ_NO, leader_committed_by_majority, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_to_commit));
	}
	NFOUT;
}

#define UPD_N_PEERS_COMMITTED(_peer_old_idx_val, _new_buf_, _which_ctx, _which_section) ({									\
	bool										_is_matching_leader_to_commit;												\
	struct nvmeibt_persist_and_wire_buf			*_new_buf = (struct nvmeibt_persist_and_wire_buf *)_new_buf_;				\
	/* Either we just reset (zeroed) leader_n_peers_committed and counting the members that matches */						\
	/* Or, we just received a buf from a member, and look for a value that is new and matches 		*/						\
	if (_new_buf) {																											\
		const int64_t new_val = nvmeibt_tlv_get_idx(&(_new_buf->_which_ctx));												\
		_is_matching_leader_to_commit = (new_val != _peer_old_idx_val) && 													\
			(new_val == RAFT_COMMIT_LIFECYCLE_VAL(_which_section, leader_to_commit));										\
	} else {																												\
		_is_matching_leader_to_commit = (_peer_old_idx_val == RAFT_COMMIT_LIFECYCLE_VAL(_which_section, leader_to_commit));	\
	}																														\
	if (_is_matching_leader_to_commit) {																					\
		my_raft_global._which_section##_commit_lifecycle.leader_n_peers_committed++;										\
	}																														\
})

static void raft_leader_reset_counters_upon_last_LOG_change(void)
{
	struct nvmeibt_raft_member					*peer;
	struct nvmeibt_persist_and_wire_buf	*peer_buf;

	NFIN;
	SET_RAFT_COMMIT_LIFECYCLE_VAL(csujw02, TOPO,              leader_n_peers_committed, 0);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(0h27ska, TOPO_CONFIG,       leader_n_peers_committed, 0);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(irncah5, KAFKA_MGMT_CONFIG, leader_n_peers_committed, 0);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(virypm4, RAFT_MEMBERS,      leader_n_peers_committed, 0);
	NVMEIB_HASH_FOREACH(peer, my_raft_global.raft_members_hash_by_uuid) {
		peer_buf = &(peer->committed_persist_and_wire_buf_hdr);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(peer_buf->topo_ctx)),              NULL, topo_ctx,              TOPO);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(peer_buf->topo_config_ctx)),       NULL, topo_config_ctx,       TOPO_CONFIG);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(peer_buf->kafka_mgmt_config_ctx)), NULL, kafka_mgmt_config_ctx, KAFKA_MGMT_CONFIG);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(peer_buf->raft_members_ctx)),      NULL, raft_members_ctx,      RAFT_MEMBERS);
	}
	raft_leader_check_committed_by_majority_and_act_upon();
	NFOUT;
}

// When a peer reports (replies to my topo !!!) of a last_log, the leader considers it committed-to enough.
// We consider it committed when all 4 sections are committed (anyhow, they are persisted at once).
static void raft_leader_update_committed_values_of_a_peer(struct nvmeibt_raft_member *peer, const struct nvmeibt_persist_and_wire_buf *new_buf)
{
	struct nvmeibt_persist_and_wire_buf		*member_buf;

	NFIN;
	member_buf = &(peer->committed_persist_and_wire_buf_hdr);
	if (compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(member_buf, new_buf) != PERSIST_AND_WIRE_BUF_DIFF_EQUAL) {
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(member_buf->topo_ctx)),              new_buf, topo_ctx,              TOPO);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(member_buf->topo_config_ctx)),       new_buf, topo_config_ctx,       TOPO_CONFIG);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(member_buf->kafka_mgmt_config_ctx)), new_buf, kafka_mgmt_config_ctx, KAFKA_MGMT_CONFIG);
		UPD_N_PEERS_COMMITTED(nvmeibt_tlv_get_idx(&(member_buf->raft_members_ctx)),      new_buf, raft_members_ctx,      RAFT_MEMBERS);
		raft_leader_check_committed_by_majority_and_act_upon();
		peer->committed_persist_and_wire_buf_hdr = *new_buf;	// Update all at once
	}
	NFOUT;
}

static void raft_write_leader_name(char *leader_name)
{
	struct leader_name_wq_entry *write_leader_name_task;

	NFIN;

	nvmeibt_strlcpy(my_raft_global.leader_node_name, leader_name, sizeof(my_raft_global.leader_node_name));
	// Allocate task to offload writing leader_name to a thread.
	write_leader_name_task = NNVMEIBT_BM_CALLOC(trace_raft_raft_write_leader_name, sizeof(*write_leader_name_task));
	write_leader_name_task->wq_entry.type = "SAVE_LEADER_NAME";
	write_leader_name_task->wq_entry.execute = write_leader_name_to_file_wrapper;
	write_leader_name_task->wq_entry.free = write_leader_name_to_file_freer;

	/*
	 * Copy the node name to the offloading task, in case for some reason the
	 * leader node will be deleted, when we are doing the actual write.
	 */
	nvmeibt_strlcpy(write_leader_name_task->leader_node_name, leader_name, sizeof(write_leader_name_task->leader_node_name));
	if (strlen(leader_name) > NVMEIB_HOST_NAME_LEN) {
		N_Ef(error_raft_raft_write_leader_name, "node_name is too long! len=@LEN_SIZET max_allowed=@MAX_ALLOWED", strlen(leader_name), NVMEIB_HOST_NAME_LEN);
		NNVMEIBT_BM_FREE(trace_1_raft_raft_write_leader_name, write_leader_name_task);
		goto out;
	}
	if (nvmeibt_toma_leader_add_work(&(write_leader_name_task->wq_entry)) != 0) {
		N_Ef(error_1_raft_raft_write_leader_name, "Unable to add leader name offload task to WQ!");
		NNVMEIBT_BM_FREE(trace_2_raft_raft_write_leader_name, write_leader_name_task);
		goto out;
	}

out:
	NFOUT;
}

/*************************      Leader history      ***************************/

struct leader_history_entry {
	int						timestamp_sec;
	union nvmeib_uuid		leader_uuid;
};

static struct leader_history_entry	leader_history_array[10];
static int							n_changes_in_leader_history_array = 0;		// Never reset

int nvmeibt_raft_get_avg_leader_lifespan_sec(void)
{
	int					oldest_entry_idx = n_changes_in_leader_history_array % ARRAY_SIZE(leader_history_array);
	struct timespec		now;

	getnstimeofday_boot(&now);
	return (n_changes_in_leader_history_array >= ARRAY_SIZE(leader_history_array) ?
			(now.tv_sec - leader_history_array[oldest_entry_idx].timestamp_sec) / ARRAY_SIZE(leader_history_array) :
			INT_MAX);
}

static void add_entry_to_leader_history_array(int timestamp_sec, const union nvmeib_uuid *leader_uuid)
{
	int		idx_in_arr;

	idx_in_arr = (n_changes_in_leader_history_array) % ARRAY_SIZE(leader_history_array);
	leader_history_array[idx_in_arr].timestamp_sec = timestamp_sec;
	leader_history_array[idx_in_arr].leader_uuid = *leader_uuid;
	n_changes_in_leader_history_array++;
}

/***********************                             **************************/

void nvmeibt_raft_set_leader_uuid(const union nvmeib_uuid *leader_uuid)
{
	bool								was_there_a_leader = raft_do_we_have_a_leader();

	NFIN;

	if (!leader_uuid)
		leader_uuid = &nvmeib_uuid_null_val;

	if (raft_is_leader_uuid(leader_uuid))
		goto out;	// Same leader

	my_raft_global.leader_uuid = *leader_uuid;
	if (!raft_do_we_have_a_leader()) {  // After assigning the new value
		N_Tf(5gsuaw4, "new LEADER=NULL. The current topology is still active");
	} else {
		add_entry_to_leader_history_array(nvmeibt_global_get_cur_event_start_time().tv_sec, leader_uuid);
		NVMEIBT_IMPORTANT_LOGS_NEW_LEADER(sim6bae, raft_get_leader_node_name());
		if (!was_there_a_leader) {
			nvmeibt_toma_raft_validity_was_updated();
		}
	}

	/* if replay: skip (only do leader's logic and topology calculation) */
	if (nvmeibt_replay_is_enabled())
		goto out;
	if (nvmeibt_toma_is_running_as_a_utility()) {
		goto out;
	}

	my_raft_global.time_converted_to_leader = nvmeibt_global_get_cur_event_start_time();
	raft_write_leader_name(raft_get_leader_node_name());

out:
	NFOUT;
}

static inline void add_noise_to_srand48(void)
{
	unsigned randseed;
	struct timespec ts;

	// sys_getrandom(&randseed, sizeof randseed, 0);
	getnstimeofday_boot(&ts);
	randseed = ts.tv_nsec;
	srand48(lrand48() ^ randseed);	// Even the slightest change makes a total difference
}

static void raft_reset_election_timeout(BOOL is_just_allowing_busy_main_loop_time_to_receive_leader_heartbeat)
{
	struct timespec		now_plus_little;

	NFIN;
	getnstimeofday_boot(&now_plus_little);
	if (!is_just_allowing_busy_main_loop_time_to_receive_leader_heartbeat) {
		add_noise_to_srand48();   // Otherwise raft on different nodes use the same seed and are synchronized
		my_raft_global.next_election_time = now_plus_little;
		timespec_update_by_a_few_nsec(&(my_raft_global.next_election_time), RAFT_RANDOM_ELECTION_TIMEOUT_NSEC);
	}
	// In any case, due to too long handling time, allow some processing time (avoid thrashing due to immediate timeout)
	timespec_update_by_a_few_nsec(&now_plus_little,
							  (is_just_allowing_busy_main_loop_time_to_receive_leader_heartbeat ? raft_leader_heartbeat_timeout_nsec * 2 : raft_leader_heartbeat_timeout_nsec / 2));	// Allow time an event other than timeout
	my_raft_global.next_election_time = timespec_max(my_raft_global.next_election_time, now_plus_little);
	NFOUT;
}

/*********    voted_for_me   *************/

static void mark_that_we_just_heard_from_peer(struct nvmeibt_raft_member *peer)
{
	if (!(peer->is_alive_for_topo)) {
		peer->is_alive_for_topo = 1;
		peer->is_alive_for_topo_start_timespec = nvmeibt_global_get_cur_event_start_time();
	}
	peer->last_received_voted_for_me_timespec = nvmeibt_global_get_cur_event_start_time();
}

static void leader_handle_new_vote(struct nvmeibt_raft_member *peer)
{
	if (!peer) {
		N_Tf(jhjhu88, "peer=NULL, skipping");
	} else {
		mark_that_we_just_heard_from_peer(peer);
		N_Tf(ywue172, "new vote received from peer=@STR prev is_vote_valid=@INT", nvmeibt_raft_member_name(peer), peer->is_vote_valid);
		if (!peer->is_vote_valid) {
			peer->is_vote_valid = 1;
			my_raft_global.n_peers_voted_for_me++;
		}
	}
}

static void leader_upd_is_vote_valid_and_is_alive_for_topo(struct nvmeibt_raft_member *peer)
{
	long long int				time_since_last_seen_nsec;

	if (!peer) {
		N_Tf(xy3hiej, "peer=NULL, skipping");
		return;
	}

	time_since_last_seen_nsec = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), peer->last_received_voted_for_me_timespec);

	// is_vote_valid processing
	if (peer->is_vote_valid) {
		peer->is_vote_valid = (time_since_last_seen_nsec < raft_max_time_leader_survives_without_majority_nsec);
		if (!peer->is_vote_valid)
			my_raft_global.n_peers_voted_for_me--;
	}

	// is_alive_for_topo processing
	if (peer->is_alive_for_topo) {
		peer->is_alive_for_topo = (time_since_last_seen_nsec < raft_max_time_non_responsive_member_is_considered_alive_for_topo_nsec);
		if (!peer->is_alive_for_topo) {
			peer->last_local_serialization_version = 0;
			nvmeibt_toma_leader_mark_member_non_responsive(peer);
		}
	}

	N_Tf(fbs88k2, "@STR: time_since_last_seen_nsec=@LLD>?@LLD is_alive_for_topo=@INT is_vote_valid=@INT", nvmeibt_raft_member_name(peer),
		time_since_last_seen_nsec, raft_max_time_non_responsive_member_is_considered_alive_for_topo_nsec, peer->is_alive_for_topo,
		peer->is_vote_valid);
}

static void raft_reset_voted_for_me(void)
{
	struct nvmeibt_raft_member	*member;

	NFIN;
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		member->is_vote_valid = 0;
		member->last_received_voted_for_me_timespec = TIMESPEC_ZERO;
	}
	my_raft_global.n_peers_voted_for_me = 0;
	NFOUT;
}

BOOL nvmeibt_raft_leader_is_peer_vote_recent(struct nvmeibt_raft_member *member)
{
	BOOL	did_we = (member && member->is_alive_for_topo);
	//FIN;
	if (!did_we && member) {
		N_Tf(k3m77ja, "Did not hear from node=@UUID_LE", nvmeibt_raft_member_name(member));
	}
	//FOUT;
	return did_we;
}

static BOOL is_majority_voted_for_me_following_new_vote(struct nvmeibt_raft_member *member)
{
	int		is_majority;

	NFIN;
	leader_handle_new_vote(member);
	is_majority = is_raft_majority();
	N_Tf(dhturie, "is_majority=@IS_MAJORITY, n_peers_voted_for_me=@N_PEERS_VOTED_FOR_ME, n_peers_config=@N_PEERS_CONFIG",
		 is_majority, my_raft_global.n_peers_voted_for_me, my_raft_global.n_raft_active_members);
	if (is_majority) {
		my_raft_global.last_time_leader_had_a_majority = nvmeibt_global_get_cur_event_start_time();
	}
	if (3 * my_raft_global.n_peers_voted_for_me > 2 * my_raft_global.n_raft_active_members) {	// 2/3 of members
		my_raft_global.leader_last_2_3rds_majority_timestamp_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;
	}
	NFOUT;
	return is_majority;
}

/*********    Follower   *************/

static void raft_convert_to_follower(const union nvmeib_uuid *voted_for_uuid, const union nvmeib_uuid *leader_uuid)
{
	NFIN;
	if (my_raft_global.role != RAFT_ROLE_FOLLOWER) {
		N_Tf(trace_raft_raft_convert_to_follower, "    [-----  RAFT FOLLOWER ------]");
		my_raft_global.time_converted_to_leader = TIMESPEC_MAX_C99;
	}

	nvmeibt_kafka_req_stop_consuming_leader_VOL_msgs();
	nvmeibt_kafka_req_stop_consuming_leader_TARGET_msgs();
	my_raft_global.role = RAFT_ROLE_FOLLOWER;
	nvmeibt_raft_set_voted_for_uuid(voted_for_uuid);
	my_raft_global.n_peers_voted_for_me = 0;
	nvmeibt_raft_set_leader_uuid(leader_uuid);

	/* inform dumper (recorder) of change of leader */
	nvmeibt_dumper_change_of_leader(false);

	NFOUT;
}

/*********    Leader   *************/

static void raft_reset_leader_heartbeat_timeout(bool is_retry_needed)
{
	N_Tf(uu111xx, "");
	my_raft_global.next_leader_heartbeat_timespec = nvmeibt_global_get_cur_event_start_time();
	timespec_update_by_a_few_nsec(&my_raft_global.next_leader_heartbeat_timespec,
									  is_retry_needed ? (raft_leader_heartbeat_timeout_nsec >> 1) : (raft_leader_heartbeat_timeout_nsec));
}

static void raft_reinitialize_after_election(void)
{
	// This is purely related to the (removed) LOG functionality
	if (!nvmeibt_raft_get_my_raft()) {
		N_Ef(error_raft_raft_reinitialize_after_election, "OOPS");
	}
}

static void raft_leader_check_validity_of_majority(void)
{
	long long int				time_since_last_majority_nsec;

	NFIN;
	time_since_last_majority_nsec = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), my_raft_global.last_time_leader_had_a_majority);
	if (time_since_last_majority_nsec > raft_max_time_leader_survives_without_majority_nsec) {
		N_Tf(nu390sl, "my_raft_global.last_time_leader_had_a_majority={@LLD.@TIMESPEC_NS}, cur_event_start_time={@LLD.@TIMESPEC_NS}, @LLD>@LLD",
			my_raft_global.last_time_leader_had_a_majority.tv_sec, my_raft_global.last_time_leader_had_a_majority.tv_nsec,
			nvmeibt_global_get_cur_event_start_time().tv_sec, nvmeibt_global_get_cur_event_start_time().tv_nsec,
			time_since_last_majority_nsec, raft_max_time_leader_survives_without_majority_nsec);
		raft_convert_to_follower(NULL, NULL);
		nvmeibt_toma_raft_validity_was_updated();
	}
	NFOUT;
}

static void raft_leader_check_validity_of_all_peers(void)
{
	struct nvmeibt_raft_member	*peer_member;

	NFIN;
	NVMEIB_HASH_FOREACH(peer_member, my_raft_global.raft_members_hash_by_uuid) {
		if (!peer_member->is_me) {
			leader_upd_is_vote_valid_and_is_alive_for_topo(peer_member);
		}
	}
	NFOUT;
}

static void set_guaranteed_sw_ver(void)
{
	struct nvmeibt_raft_member	*peer_member;
	uint32_t					new_guaranteed_sw_ver = 0;
	int							n_members = my_raft_global.n_raft_members;
	int							n_new_ver = 0;

	NVMEIB_HASH_FOREACH(peer_member, my_raft_global.raft_members_hash_by_uuid) {
		if (peer_member->toma_software_version > my_raft_global.guaranteed_sw_ver) {
			new_guaranteed_sw_ver = peer_member->toma_software_version;
			N_Tf(u87b443, "peer=@STR, has higher SW ver=@SOFTWARE_VERSION", peer_member->hostname, new_guaranteed_sw_ver);
			n_new_ver++;
		}
	}
	if (n_new_ver) {
		// Don't switch if there are not enough members with new SW
		if (IS_PERCENTAGE_EXCEEDED(n_new_ver, n_members, 66)) {
			N_Tf(k098ju7, "Try to switch to new SW version. old_ver=@SOFTWARE_VERSION, new_ver=@SOFTWARE_VERSION, n_new=@INT, n_members=@INT",
				 my_raft_global.guaranteed_sw_ver, new_guaranteed_sw_ver, n_new_ver, n_members);
			my_raft_global.guaranteed_sw_ver = new_guaranteed_sw_ver;
			SET_RAFT_LEADER_NEXT_TOPOLOGY_VERSION(cb7bq4k);
			SET_RAFT_COMMIT_LIFECYCLE_VAL(v9m4jkw, TOPO, leader_to_commit, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_calculated));
			nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
		} else {
			N_Tf(k09j227, "Not enough members with new SW, don't switch. old_ver=@SOFTWARE_VERSION, new_ver=@SOFTWARE_VERSION, n_new=@INT, n_members=@INT",
				 my_raft_global.guaranteed_sw_ver, new_guaranteed_sw_ver, n_new_ver, n_members);
		}
	}
}

static int raft_leader_send_appendentries_to_a_peer(struct nvmeibt_raft_member *dst_member, int is_with_raft_log)
{
	struct nvmeibt_topology					*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_persist_and_wire_buf		*send_persist_and_wire_buf;
	unsigned int							data_len;
	enum PERSIST_AND_WIRE_BUF_DIFF			tlv_bufs_diff;
	int										rv = 0;
	struct nvmeibt_node						*node;

	NFIN;
	if (!dst_member) {
		N_Wf(ctw824k, "dst_member=NULL");
		goto out;
	}
	// Decided to move forward, Update the header, and if needed, update the data (if is_with_raft_log and we passed the prev validation)
	raft_leader_regenerate_the_to_commit_persist_and_wire_bufs_as_needed();
	//
	// Decide what to send according to the peer's needs. HEADER_ONLY/TOPO_ONLY/FULL
	tlv_bufs_diff = compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(&(dst_member->committed_persist_and_wire_buf_hdr), my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete);
	if (memcmp(&(dst_member->committed_persist_and_wire_buf_hdr.raft_ctx), &(my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete->raft_ctx), sizeof(dst_member->committed_persist_and_wire_buf_hdr.raft_ctx)) != 0) {
		N_Tf(5v7hnak, "raft_ctx diff (the member committed to a different leader). For now send the full buf. When we have a d.b., send only the missing updates");
		tlv_bufs_diff = PERSIST_AND_WIRE_BUF_DIFF_NON_TOPO;
	}
	switch (tlv_bufs_diff) {
	case PERSIST_AND_WIRE_BUF_DIFF_NON_TOPO:
		// Full CONFIG
		send_persist_and_wire_buf = my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete;
		data_len = persist_and_wire_buf_get_total_len(send_persist_and_wire_buf) - sizeof(struct nvmeibt_persist_and_wire_buf);
		break;
	case PERSIST_AND_WIRE_BUF_DIFF_TOPO_ONLY:
		send_persist_and_wire_buf = my_raft_global.leader_to_commit_persist_and_wire_buf_topo_only_complete;
		data_len = persist_and_wire_buf_get_total_len(send_persist_and_wire_buf) - sizeof(struct nvmeibt_persist_and_wire_buf);
		break;
	case PERSIST_AND_WIRE_BUF_DIFF_EQUAL:
	default:
		// Only the header
		N_Tf(5basjzs, "Sending only the header. The member already has this persist_and_wire_buf_full");
		send_persist_and_wire_buf = my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete;
		data_len = 0;	// No real data
		is_with_raft_log = 0;
		break;
	}
	node = nvmeibt_raft_member_get_node(dst_member);	// Might fail
	getnstimeofday_boot(&(cur_topo->last_send_appendentries_timestamp));
	raft_send_msg_to_peer(RAFT_MSG_APPEND_ENTRIES, node, 0, is_with_raft_log, send_persist_and_wire_buf, 0, data_len);
out:
	NFOUT;
	return rv;
}

static BOOL is_raft_state_mature_and_ready_for_distribution(void) {
	BOOL	is_mature = my_raft_global.is_leader_mature ||
					(my_raft_global.n_peers_voted_for_me == my_raft_global.n_raft_active_members) ||
					(nvmeibt_global_get_cur_event_start_time().tv_sec - nvmeibt_global_get_startup_timespec().tv_sec >
					 raft_time_from_startup_to_activation_sec);

	if (is_mature) {
		if (!my_raft_global.is_leader_mature) {
			my_raft_global.is_leader_mature = 1;
			nvmeibt_kafka_req_start_consuming_leader_VOL_msgs(RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed) + 1, nvmeibt_raft_get_current_term());
			nvmeibt_kafka_req_start_consuming_leader_TARGET_msgs(
				RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed) + 1, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed), nvmeibt_raft_get_current_term());
			N_Tf(ry76493, " LEADER mature.");
		}
	}
	else {
		static int count = 0;
		if (!(0xf & count++)) {
			N_Tf(cc884nw, "Not mature yet.");
		}
	}
	return my_raft_global.is_leader_mature;
}



static bool is_srm_ready_to_accept_the_new_msgs(bool is_with_raft_log)
{
	int							rv;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_node			*node;
	struct timespec				now;
	long long int				diff_timeout_nsec;
	int int_tm_cnt = atomic_read(&cur_topo->in_transmission_cnt);

	NFIN;
	// SRM copies the header, but not the data, hence we are not allowed to modify the data when we have a transmission in the air
	if (	!int_tm_cnt ||
			!is_with_raft_log ||
			!(cur_topo->is_update_csv_of_config_and_topo_required)) {
		// This msg will not override the data of an active transmission
		rv = 1;
		goto out;
	}
	getnstimeofday_boot(&now);
	diff_timeout_nsec = timespec_diff_ns(now, cur_topo->last_send_appendentries_timestamp);
	if (diff_timeout_nsec < (raft_leader_heartbeat_timeout_nsec * RAFT_TX_WAIT_FACTOR)) {
		N_Tf(c63b3ks, "Skipping. Awaiting previous transmission to finished, tx_remained=@INT, timeout=@INT64 ms", int_tm_cnt, NSEC_TO_MSEC(diff_timeout_nsec));
		rv = 0;
		goto out;
	}
	N_Tf(ysb38kw, "Timeout expired. Canceling all prev sends. tx_remained=@INT, timeout=@INT64 ms", int_tm_cnt, NSEC_TO_MSEC(diff_timeout_nsec));
	NVMEIB_HASH_FOREACH(node, cur_topo->nodes_hash_by_uuid) {
		if (!nvmeibt_node_is_my_node(node)) {
			nvmeibt_node_cancel_send(node);
		}
	}
	int_tm_cnt = atomic_read(&cur_topo->in_transmission_cnt);	// Reread updated value
	if (int_tm_cnt) {
		N_Tf(vn92msl, "SRM not ready yet. cancel did not finish its async work. in_transmission_cnt=@INT", int_tm_cnt);
		rv = 0;
		goto out;
	}
	rv = 1;
out:
	NFOUT;
	return rv;
}

static int raft_leader_send_appendentries_to_all_peers(int is_with_raft_log)
{
	int							rv = 0;
	struct nvmeibt_raft_member	*member;
	int							is_prev_topo_committed = (RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority) == RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit));
	static int64_t				prev_topo_to_commit = -1;
	static int64_t				prev_topo_to_apply = -1;
	bool						is_new_committed_or_applied;

	NFIN;
	// APPEND_ENTRIES also serves as the leader's heartbeat
	//  Its name is derived from append entries to LOG, which we ignore for now.
	if (is_with_raft_log) {	// Ignoring the first appendentries that the leader sends
		raft_leader_check_validity_of_majority();	// Might void my leadership
		if (my_raft_global.role != RAFT_ROLE_LEADER) {
			goto out;
		}
	}
	is_with_raft_log &= is_raft_state_mature_and_ready_for_distribution();
	// Recalc the topology if needed, and the prev topo was applied
	if (is_with_raft_log && is_prev_topo_committed) {
		if (TOMA_SW_COMPATIBILITY_VER < nvmeibt_raft_get_guaranteed_sw_ver()) {
			N_Tf(h4shek3, "SW ver=@SOFTWARE_VERSION is old, must be ver=@SOFTWARE_VERSION",
				 TOMA_SW_COMPATIBILITY_VER, nvmeibt_raft_get_guaranteed_sw_ver());
			raft_convert_to_follower(NULL, NULL);
			// I want to be a leader in the case of emergency only, so set a long election timeout
			my_raft_global.next_election_time = nvmeibt_global_get_cur_event_start_time();
			timespec_update_by_a_few_nsec(&(my_raft_global.next_election_time), RAFT_RANDOM_ELECTION_TIMEOUT_NSEC * 2);
			goto out;
		}
		my_raft_global.is_leader_ever_committed_by_majority = 1;
		set_guaranteed_sw_ver();
		nvmeibt_topology_calc_topology();
	} else {
		N_Tf(c5dv398, "is_with_raft_log=@BOOL is_prev_topo_committed=@BOOL", is_with_raft_log, is_prev_topo_committed);
	}
	my_raft_global.last_tx_append_entries_msg_num++;
	if (is_srm_ready_to_accept_the_new_msgs(is_with_raft_log)) {
		getnstimeofday_boot(&(nvmeibt_global_get_global()->last_raft_distribution_timestamp));
		is_new_committed_or_applied = (prev_topo_to_commit != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit) ||
									   prev_topo_to_apply != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority));
		if (is_new_committed_or_applied) {
			prev_topo_to_commit = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit);
			prev_topo_to_apply = RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority);
			my_raft_global.leader_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec = nvmeibt_global_get_global()->last_raft_distribution_timestamp;
		}
		NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
			struct nvmeibt_node	*its_node = nvmeibt_raft_member_get_node(member);
			if (its_node) {
				raft_leader_send_appendentries_to_a_peer(member, is_with_raft_log);
				member->its_node->peer_statistics.is_awaiting_REP_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec |= is_new_committed_or_applied;
			}
		}
		raft_reset_leader_heartbeat_timeout(false);
	} else {
		raft_reset_leader_heartbeat_timeout(true);
	}
out:
	NFOUT;
	return rv;
}

static int raft_leader_timeout(void)
{
	int		rv = 0;

	NFIN;
	if (my_raft_global.role != RAFT_ROLE_LEADER) {
		goto out;
	}
	raft_leader_check_validity_of_all_peers();
	rv = raft_leader_send_appendentries_to_all_peers(1);
out:
	NFOUT;
	return rv;
}

static void leader_reset_peers_committed_values(void)
{
	struct nvmeibt_raft_member					*member;

	NFIN;
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		init_persist_and_wire_buf(&(member->committed_persist_and_wire_buf_hdr));
		// member->raft_ctx.last_append_entries_rep_msg_num = 0;
	}
	NFOUT;
}

static void raft_leader_reset_all_members_raft_ctx(void)
{
	struct nvmeibt_raft_member	*member;
	NFIN;
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		raft_reset_member_ctx(member);
	}
	NFOUT;
}

static int get_n_connected_members(void)
{
	int									n_connected_members = 0;
	struct nvmeibt_nm_local_node		*local_node = nvmeibt_get_nw_node();
	struct nvmeibt_raft_member			*member;

	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		n_connected_members += nvmeibt_nm_is_remote_node_connected(local_node, member->its_node);
	}
	return n_connected_members;
}

void nvmeibt_raft_convert_to_leader(void)
{
	struct nvmeibt_raft_member	*member;

	NFIN;
	if (my_raft_global.role != RAFT_ROLE_LEADER) {	// I might already be a leader!
		NVMEIBT_IMPORTANT_LOGS_CONVERT_TO_LEADER(vjh0slw);
		//
		raft_reinitialize_after_election();
		N_Tf(hut87e4, "current_term=@RAFT_TERM (TOPO, follower_committed)=@INT64_TX", nvmeibt_raft_get_current_term(), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed));
		nvmeibt_topology_serialize_conf_and_topo_if_needed();
		my_raft_global.leader_committed_LOG_index = -1;
		my_raft_global.role = RAFT_ROLE_LEADER;
		my_raft_global.is_leader_mature = 0;
		my_raft_global.is_leader_ever_committed_by_majority = 0;
		my_raft_global.last_tx_append_entries_msg_num = 0;
		nvmeibt_disk_leader_detach_all_disks_from_raft_members();
		NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
			// Start assuming that the old global_topology is valid, and then detect disconnects
			mark_that_we_just_heard_from_peer(member);
		}
		leader_reset_peers_committed_values();
		nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
		raft_leader_reset_counters_upon_last_LOG_change();	// The standard says "reinitialize after election".
		// Done with the cleanup, now set things
		nvmeibt_raft_set_voted_for_uuid(raft_get_my_uuid());
		nvmeibt_raft_set_leader_uuid(raft_get_my_uuid());
		//
		if (nvmeibt_toma_is_running_as_a_utility()) {
			goto out;
		}
		raft_leader_send_appendentries_to_all_peers(0); // Heartbeat only - no valid topo

		/* inform dumper (recorder) of change of leader */
		nvmeibt_dumper_change_of_leader(true);
		nvmeibt_global_leader_clear_old_reports_to_mgmt();
	}
out:
	NFOUT;
}

int nvmeibt_raft_get_time_without_leader_sec(void)
{
	return (nvmeibt_global_get_cur_event_start_time().tv_sec - my_raft_global.last_recieved_append_entries_timestamp_sec);
}

/*********    Candidate   *************/

static void raft_convert_to_candidate(char flags)
{
	struct nvmeibt_raft_member		*member;
	int								n_connected_members;

	NFIN;
	// Remember, there are no stores_in_progress. The committed is stable
	// I can take votes and become a leader only if I am a member
	if (timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_get_startup_timespec()) < (raft_leader_heartbeat_timeout_nsec * 5)) {
		// When a node starts, it should first allow msgs from an existing leader.
		// When a node got disconnected, probably others got disconnected too, so not worth waiting a fixed timeout
		N_Tf(tcvsjwd, "Allowing time for leader's heartbeat. Skipping. event start time=<@LLD.@TIMESPEC_NS> startup_time=<@LLD.@TIMESPEC_NS>",
			nvmeibt_global_get_cur_event_start_time().tv_sec,
			nvmeibt_global_get_cur_event_start_time().tv_nsec,
			nvmeibt_global_get_startup_timespec().tv_sec,
			nvmeibt_global_get_startup_timespec().tv_nsec);
		goto out;
	}
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Tf(83nasj3, "!nvmeibt_topology_is_HW_config_functional");
		goto out;
	}
	nvmeibt_raft_set_leader_uuid(NULL);

	n_connected_members = get_n_connected_members();
	if (my_raft_global.n_raft_active_members &&
		((n_connected_members * 2) <= my_raft_global.n_raft_active_members)) {
		N_Tf(nxxdo49, "is not connected to majority, n_connected_members=@INT", n_connected_members);
		goto out;
	}
	// Start election
	my_raft_global.current_term++;	// Increment the current_term upon every election_timeout
	if (my_raft_global.n_raft_members == 0) {
		N_Tf(cskwu7x, "No raft_member yet. Continue as a candidate, and read raft_members from kafka");
		nvmeibt_kafka_req_start_consuming_leader_TARGET_msgs(
			RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed) + 1, RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_committed), nvmeibt_raft_get_current_term());
	}
	nvmeibt_raft_leader_copy_committed_persist_and_wire_buf_sections_into_separate_to_commit_bufs();
	//
	if (my_raft_global.role != RAFT_ROLE_CANDIDATE) {
		N_Tf(trace_2_raft_raft_convert_to_candidate, "    [-----  RAFT CANDIDATE ------]");
		my_raft_global.role = RAFT_ROLE_CANDIDATE;
		raft_leader_reset_all_members_raft_ctx();
		/* inform dumper (recorder) of change of leader */
		nvmeibt_dumper_change_of_leader(false);
	}
	if (!(my_raft_global.my_member)) {
		goto out;	// Skip the voting & peers
	}
	// Vote for myself
	nvmeibt_raft_set_voted_for_uuid(raft_get_my_uuid());	// Before filling the persist_and_topo raft_ctx
	//
	nvmeibt_topology_reset_due_to_convert_to_leader();
	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
	raft_leader_regenerate_the_to_commit_persist_and_wire_bufs_as_needed();
	//
	NVMEIB_HASH_FOREACH(member, my_raft_global.raft_members_hash_by_uuid) {
		if (!(member->is_me)) {
			raft_send_msg_to_peer(RAFT_MSG_REQ_VOTE, nvmeibt_raft_member_get_node(member), 0, 0, my_raft_global.leader_to_commit_persist_and_wire_buf_full_complete, flags, 0);  // Only the header
		}
	}
	raft_reset_voted_for_me();
	if (is_majority_voted_for_me_following_new_vote(my_raft_global.my_member)) {
		nvmeibt_raft_convert_to_leader();
	}
out:
	raft_reset_election_timeout(0);	// All code-paths really reset the raft timeout
	NFOUT;
}

/*********    Timeout   *************/

struct timespec nvmeibt_raft_get_next_timeout_timespec(void)
{
	// FIN;
	// FOUT;
	return (my_raft_global.role == RAFT_ROLE_LEADER ?
			my_raft_global.next_leader_heartbeat_timespec : my_raft_global.next_election_time);
}

int nvmeibt_raft_timeout_occurred(BOOL did_pselect_allow_time_to_receive_append_entries,  bool is_forced_reelect)
{
	int							rv = 0;

	NFIN;
	if (my_raft_global.role == RAFT_ROLE_LEADER) {
		if (timespec_ge(nvmeibt_global_get_cur_event_start_time(), my_raft_global.next_leader_heartbeat_timespec)) {	// Leader heartbeat timeout
			rv = raft_leader_timeout();
		}
	}
	else {	// For FOLLOWER/CANDIDATE
		N_Tf(trace_raft_nvmeibt_raft_timeout_occurred, "Election_timeout expired - Checking whether I can become a candidate. event start time:  <@LLD.@TIMESPEC_NS> next election time:  <@LLD.@TIMESPEC_NS>, is_sufficient_time_gap_before_raft = @IS_SUFFICIENT_TIME_GAP_BEFORE_RAFT",
			nvmeibt_global_get_cur_event_start_time().tv_sec,
			nvmeibt_global_get_cur_event_start_time().tv_nsec,
			my_raft_global.next_election_time.tv_sec,
			my_raft_global.next_election_time.tv_nsec,
			did_pselect_allow_time_to_receive_append_entries);
		if (is_forced_reelect || timespec_ge(nvmeibt_global_get_cur_event_start_time(), my_raft_global.next_election_time)) {
			// Want to convert_to_candidate
			if (	did_pselect_allow_time_to_receive_append_entries &&
					nvmeibt_global_get_n_stores_in_progress() == 0) {	// We do not want to become candidates in the gap between submit and commit to persistence. too confusing. Say when a raft member was added.
				N_Tf(trace_1_raft_nvmeibt_raft_timeout_occurred, "Election_timeout expired, converting to candidate");
				raft_convert_to_candidate(is_forced_reelect ? FORCED_REELECT_RAFT_FLG : 0);
			}
			else {
				raft_reset_election_timeout(1);	//  We did not allow enough processing time for the main select(). Add some.
			}
		}
	}
	NFOUT;
	return rv;
}

/*********    Receiver   *************/

static int leader_process_peer_msg_data(const struct raft_msg *msg, struct nvmeibt_raft_member *src_member)
{
	int											rv = 0;
	long long int								serialization_version_diff;
	bool										is_with_remote_applied_topo_data = 0;
	int											applied_topo_len;
	char										*applied_topo_data;
	int											total_len;

	NFIN;
	total_len = persist_and_wire_buf_get_total_len(&(msg->persist_and_wire_buf));
	N_Tf(tbbbb2n, "TOPO_len=@INT total_data_len=@INT", nvmeibt_tlv_get_len(&(msg->persist_and_wire_buf.topo_ctx)), total_len - (int)sizeof(msg->persist_and_wire_buf));
	is_with_remote_applied_topo_data = msg->is_with_raft_log && (nvmeibt_tlv_get_len(&(msg->persist_and_wire_buf.topo_ctx)) > 0);

	serialization_version_diff = msg->local_serialization_version - src_member->last_local_serialization_version;
	if (persist_and_wire_buf_get_last_rx_append_entries_raft_TERM(&(msg->persist_and_wire_buf)) != nvmeibt_raft_get_current_term()) {
		N_Tf(mwu7z5a, "member_committed_term=@LLX < @LLX, ignoring the data", persist_and_wire_buf_get_current_raft_TERM(&(msg->persist_and_wire_buf)), nvmeibt_raft_get_current_term());
		goto skip_data_processing;
	}
	//
	if ((serialization_version_diff != 0) && is_with_remote_applied_topo_data) {
		src_member->last_local_serialization_version = msg->local_serialization_version;
		applied_topo_len = nvmeibt_raft_get_data_from_persist_and_wire_buf_by_tlv_type(&(msg->persist_and_wire_buf), TLV_TYPE_TOPO_COMPLETE, &applied_topo_data);
		if (applied_topo_len) {
			rv = nvmeibt_topology_leader_new_remote_applied_topology_arrived(applied_topo_data, applied_topo_len, src_member);
		}
	}
	// Upd the remote committed indices
	src_member->committed_persist_and_wire_buf_hdr.raft_ctx = msg->persist_and_wire_buf.raft_ctx;
	src_member->committed_persist_and_wire_buf_hdr.buf_sw_ver = msg->persist_and_wire_buf.buf_sw_ver;
	if (compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(&(src_member->committed_persist_and_wire_buf_hdr), &(msg->persist_and_wire_buf)) != PERSIST_AND_WIRE_BUF_DIFF_EQUAL) {
		raft_leader_update_committed_values_of_a_peer(src_member, &(msg->persist_and_wire_buf));
	}
skip_data_processing:
	NFOUT;
	return rv;
}

enum RAFT_FOLLOWER_REP_RV {
	RAFT_FOLLOWER_REP_RV_INVALID = 0,
	RAFT_FOLLOWER_REP_RV_TRUE = 1,
	RAFT_FOLLOWER_REP_RV_FALSE = 2,
	RAFT_FOLLOWER_REP_RV_DEFERRED = 3,
	RAFT_FOLLOWER_REP_RV_IGNORE = 4
};

static enum RAFT_FOLLOWER_REP_RV raft_follower_persist_due_to_incoming_msg_as_needed(struct raft_msg *msg, struct nvmeibt_node *src_node, bool is_req_vote)
{
	enum RAFT_FOLLOWER_REP_RV				rep_rv;
	bool									is_raft_protocol_persist_required;
	bool									is_msg_data_persist_required;
	enum PERSIST_AND_WIRE_BUF_DIFF			persist_and_wire_buf_comparison_result;
	int										peer_eligibility_for_leader;
	struct nvmeibt_persist_and_wire_buf		*to_commit_buf = my_raft_global.follower_to_commit_persist_and_wire_buf_full;

	NFIN;
	peer_eligibility_for_leader = check_peer_eligibility_for_leader(msg, src_node, is_req_vote);
	if (peer_eligibility_for_leader < 0) {
		N_Tf(cvakew3, "!is_peer_eligible_for_leader");
		rep_rv = RAFT_FOLLOWER_REP_RV_FALSE;
		goto out;
	}
	if (peer_eligibility_for_leader > 0) {
		N_Tf(diukwn4, "Ignoring msg");
		rep_rv = RAFT_FOLLOWER_REP_RV_IGNORE;
		goto out;
	}
	is_raft_protocol_persist_required = (memcmp(&(msg->persist_and_wire_buf.raft_ctx), &(to_commit_buf->raft_ctx), sizeof(msg->persist_and_wire_buf.raft_ctx)) != 0) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(current_raft_TERM, follower_submitted) != (int64_t)persist_and_wire_buf_get_current_raft_TERM(to_commit_buf));
	persist_and_wire_buf_comparison_result = compare_persist_and_wire_bufs_tlvs_excl_raft_ctx(&(msg->persist_and_wire_buf), to_commit_buf);
	is_msg_data_persist_required = (msg->is_with_raft_log && (persist_and_wire_buf_comparison_result != PERSIST_AND_WIRE_BUF_DIFF_EQUAL)) ||
		(msg->persist_and_wire_buf.buf_sw_ver != to_commit_buf->buf_sw_ver) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_submitted) != nvmeibt_tlv_get_idx(&(to_commit_buf->topo_ctx))) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_submitted) != nvmeibt_tlv_get_idx(&(to_commit_buf->topo_config_ctx))) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_submitted) != nvmeibt_tlv_get_idx(&(to_commit_buf->kafka_mgmt_config_ctx))) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_submitted) != nvmeibt_tlv_get_idx(&(to_commit_buf->raft_members_ctx))) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, follower_submitted) != nvmeibt_tlv_get_seq_no(&(to_commit_buf->raft_members_ctx)));
	//
	if (is_req_vote) {
		// Patch the last_rx_append_entries_raft_TERM before comparing with the already existing real one
		persist_and_wire_buf_set_voted_for_and_last_rx_append_entries_raft_TERM(&(msg->persist_and_wire_buf), nvmeibt_node_UUID(src_node), my_raft_global.last_rx_append_entries_term, 1);
	} else {
		// For append entries, use the value that arrived
	}
	//
	if (!is_raft_protocol_persist_required && !is_msg_data_persist_required) {
		rep_rv = RAFT_FOLLOWER_REP_RV_TRUE;
		goto out;
	}
	nvmeibt_raft_set_voted_for_uuid(nvmeibt_node_UUID(src_node));   // It might be that we are already followers, so no convert_to_follower(), and we decided to accept a REQ_VOTE or an APPEND_ENTRIES
	// Need to persist, maybe already in work
	rep_rv = RAFT_FOLLOWER_REP_RV_DEFERRED;	// Not true since we didn't persist yet. False will mislead the leader that a better leader is in the air
	if (nvmeibt_global_get_n_stores_in_progress() > 0) {
		// Simply send a heartbeat and don't try to parse any data, since we are in the middle
		// of storing the current state on disk, and wouldn't want to ruin it.
		N_Tf(ahu88ie, "n_stores_in_progress, no re-persist for now");
		goto out;
	}
	// upd follower_to_commit_persist_and_wire_buf_full
	to_commit_buf = my_raft_global.follower_to_commit_persist_and_wire_buf_full =
			realloc_and_upd_follower_persist_and_wire_bufs_with_incoming_data(to_commit_buf, &(msg->persist_and_wire_buf), msg->is_with_raft_log);

	if (!nvmeibt_topology_add_persistency_save_wq_item(is_req_vote)) {	// In order to:
																		// - write the metadata of added/removed local segs on the NVME disks
																		// - Persist config & topo on the system disk
																		// - persist the raft-term and voted_for_member

		// Mark new submitted values
		SET_RAFT_COMMIT_LIFECYCLE_VAL(cvgher8, TOPO,                follower_submitted, nvmeibt_tlv_get_idx(&(to_commit_buf->topo_ctx)));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(5bs7k29, TOPO_CONFIG,         follower_submitted, nvmeibt_tlv_get_idx(&(to_commit_buf->topo_config_ctx)));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(shf84m2, KAFKA_MGMT_CONFIG,   follower_submitted, nvmeibt_tlv_get_idx(&(to_commit_buf->kafka_mgmt_config_ctx)));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(vbsk3oc, RAFT_MEMBERS,        follower_submitted, nvmeibt_tlv_get_idx(&(to_commit_buf->raft_members_ctx)));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(1j0sfvu, RAFT_MEMBERS_SEQ_NO, follower_submitted, nvmeibt_tlv_get_seq_no(&(to_commit_buf->raft_members_ctx)));
		SET_RAFT_COMMIT_LIFECYCLE_VAL(sku73eb, current_raft_TERM,   follower_submitted, (int64_t)persist_and_wire_buf_get_current_raft_TERM(to_commit_buf));
	}
out:
	NFOUT;
	return rep_rv;
}

static int raft_handle_req_vote(struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	enum RAFT_FOLLOWER_REP_RV	rep_rv;

	NFIN;
	rep_rv = raft_follower_persist_due_to_incoming_msg_as_needed(msg, src_node, 1);
	if (rep_rv != RAFT_FOLLOWER_REP_RV_FALSE) {
		nvmeibt_toma_raft_validity_was_updated();
		raft_reset_election_timeout(0);	// grant_vote is also a heartbeat
	}
	if (rep_rv != RAFT_FOLLOWER_REP_RV_DEFERRED) {
		TODO(Remove the if, and send a negative answer also if deferred. With proper "candidate" behavior, it will just look for a majority of vote_granted, ignoring the negatives);
		raft_send_msg_to_peer(RAFT_MSG_REQ_VOTE_REP, src_node, (rep_rv == RAFT_FOLLOWER_REP_RV_TRUE), 0, my_raft_global.follower_to_leader_wire_buf, 0, 0);
	}
	NFOUT;
	return 0;
}

static int raft_handle_req_vote_rep(struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	struct nvmeibt_raft_member *src_member = nvmeibt_node_get_raft_member(src_node);
	NFIN;
	if (my_raft_global.role == RAFT_ROLE_FOLLOWER) {
		goto out;
	}
	if (!src_member) {
		N_Tf(suc9v4d, "Got reply from a non-raft_member, ignoring");
		goto out;
	}
	if (msg->is_vote_granted) {
		// During election we just count positive votes for majority, thus !is_vote_granted messages are ignored
		leader_process_peer_msg_data(msg, src_member);
		raft_leader_update_committed_values_of_a_peer(src_member, &(msg->persist_and_wire_buf));
//		raft_leader_update_kafka_offset_committed_of_a_peer(src_member, msg->kafka_offset_commit);
		if (is_majority_voted_for_me_following_new_vote(src_member)) {
			nvmeibt_raft_convert_to_leader();
		}
	}

out:
	NFOUT;
	return 0;
}

int debug_assist_discard_some_append_entries_msgs(const struct raft_msg *msg, bool is_with_new_data)
{
	int		rv = 0;

	if (msg->is_with_raft_log && is_with_new_data) { // Received a config and/or topology
    	if (raft_num_of_messages_to_discard_after_change_tmp) {
			raft_num_of_messages_to_discard_after_change_tmp--;
			if (raft_num_of_messages_to_discard_after_change_tmp) {
				N_Tf(x_tt_40, "Ignore APPEND_ENTRIES message, discard @INT following messages", raft_num_of_messages_to_discard_after_change_tmp - 1);
				rv = -1;
			}
    		else {
				if (is_discard_after_change_permanent) {
					N_Tf(x_tt_41, "recharge ignoring to @INT messages", raft_num_of_messages_to_discard_after_change);
					raft_num_of_messages_to_discard_after_change_tmp = raft_num_of_messages_to_discard_after_change + 1;
				}
			}
		}
	}
	return rv;
}

void nvmeibt_raft_apply_committed_config_and_topo_as_needed(void)
{
	NFIN;

	// Phase 2. Apply the new conf and topo if asked for

	// First of all check if conf and topo are ready to apply
	if ((RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_to_apply) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed)) ||		// We can only apply the committed
		(RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_submitted))) {		// Actually we have the SUBMITTED data in the memory, but want to apply the committed
		N_Tf(u44492x, "topo is not ready to apply (TOPO, follower_to_apply)=@INT64_TX (TOPO, follower_committed)=@INT64_TX (TOPO, follower_submitted)=@INT64_TX",
			RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_to_apply), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_submitted));
		goto out;
	}

	if ((RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_to_apply) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed)) ||		// We can only apply the committed
		(RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_submitted))) {		// Actually we have the SUBMITTED data in the memory, but want to apply the committed
		N_Tf(u44492y, "config is not ready to apply (TOPO_CONFIG, follower_to_apply)=@INT64_TX (TOPO_CONFIG, follower_committed)=@INT64_TX (TOPO_CONFIG, follower_submitted)=@INT64_TX",
			RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_to_apply), RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed), RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_submitted));
		goto out;
	}

	//Apply the config and topo if leader ask for it
	if ((RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed)) ||
		(RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied) != RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed))) {
		N_Tf(uyww6xx, "apply: (TOPO, follower_applied)=@INT64_TX (TOPO, follower_committed)=@INT64_TX "
			 "(TOPO_CONFIG, follower_applied)=@INT64_TX (TOPO_CONFIG, follower_committed)=@INT64_TX",
			 RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed),
			 RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_committed));
		nvmeibt_topology_apply_the_latest_committed_config_and_topo();
	}
	else {
		N_Tf(7hndlwd, "nothing to apply. (TOPO, follower_applied)=@INT64_TX (TOPO_CONFIG, follower_applied)=@INT64_TX",
			 RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied), RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, follower_applied));
	}

out:
	NFOUT;
}

void nvmeibt_raft_try_to_send_spurious_APPEND_ENTRIES_REP_or_REQ_VOTE_REP(struct nvmeibt_node *dst_node, bool is_req_vote)
{
	struct nvmeibt_Buf						*applied_topo_buf = &(nvmeibt_global_get_global()->buf_of_follower_wire_topo);

	NFIN;
	if (dst_node) {
		nvmeibt_topology_serialize_active_topology();
		if (	(is_applied_topo_ready_and_different(applied_topo_buf) ||
				 RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_sent_to_leader) != RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed))) {
			raft_send_msg_to_peer((is_req_vote ? RAFT_MSG_REQ_VOTE_REP : RAFT_MSG_APPEND_ENTRIES_REP),
								  dst_node,
								  1,	// is_vote_granted
								  !is_req_vote,	// is_sending_active_topo,
								  my_raft_global.follower_to_leader_wire_buf,
								  0,
								  persist_and_wire_buf_get_total_len(my_raft_global.follower_to_leader_wire_buf) - sizeof(struct nvmeibt_persist_and_wire_buf));
		}
	}
	NFOUT;
}

static int raft_handle_append_entries(struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	enum RAFT_FOLLOWER_REP_RV					rep_rv;
	int											rv = 0;
	struct nvmeibt_topology						*cur_topo = nvmeibt_global_get_global();
	BOOL										is_sending_applied_topo;

	NFIN;
	rep_rv = raft_follower_persist_due_to_incoming_msg_as_needed(msg, src_node, 0);
	if (rep_rv == RAFT_FOLLOWER_REP_RV_FALSE) {
		N_Tf(ybskwis, "APPEND_ENTRIES replies false, probably I was a candidate for a long time");
		// Will cause the leader et al to start new elections
		goto send_rep;
	}
	if (rep_rv == RAFT_FOLLOWER_REP_RV_IGNORE) {
		goto out;
	}
	if (msg->is_with_raft_log) {
		N_Tf(rcg1obh, "Received calculated leader_append_entries_time_ns=@INT64_TD leader_topo_calc_time_ns=@INT64_TD",
			 persist_and_wire_buf_get_raft_calculated_leader_append_entries_rep_time_ns(&(msg->persist_and_wire_buf)),
			 persist_and_wire_buf_get_raft_calculated_leader_topo_calc_time_ns(&(msg->persist_and_wire_buf)));
	}
	// DEFERRED or TRUE
	my_raft_global.leader_shutdown_term = max(my_raft_global.leader_shutdown_term, msg->shutdown_term);	// Received shutdown from leader;
	SET_RAFT_COMMIT_LIFECYCLE_VAL(rvzxywu, TOPO,         follower_to_apply, msg->applied_TOPO_idx);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(3bjsiow, TOPO_CONFIG,  follower_to_apply, msg->applied_TOPO_CONFIG_idx);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(9h5jldm, RAFT_MEMBERS, follower_to_apply, msg->applied_raft_members_offset);
	if (rep_rv == RAFT_FOLLOWER_REP_RV_DEFERRED) {
		goto send_rep;
	}

	// RAFT_FOLLOWER_REP_RV_TRUE - accept_msg:
	if (!(my_raft_global.first_append_entries_term)) {
		my_raft_global.first_append_entries_term = msg->current_term;
	}
	my_raft_global.last_recieved_append_entries_timestamp_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;
	if (!nvmeibt_node_is_my_node(src_node)) {
		raft_convert_to_follower(nvmeibt_node_UUID(src_node), nvmeibt_node_UUID(src_node));
	}

	cur_topo->known_to_leader_local_serialization_version = msg->local_serialization_version;

	//  Parse the incoming config all the way (add segs etc.)
	//  Parse the new topo into committed_topology
	nvmeibt_raft_apply_committed_config_and_topo_as_needed();

	// Now, that parsed, I continue only if I am a member
	if (!(my_raft_global.my_member)) {
		N_Wf(byw93kn, "I am not a raft_member after parsing the incoming msg. Ignoring");
		rep_rv = RAFT_FOLLOWER_REP_RV_FALSE;
		goto out;
	}

	if (debug_assist_discard_some_append_entries_msgs(msg, (rep_rv == RAFT_FOLLOWER_REP_RV_TRUE)) < 0) {
		goto out;
	}

	// If we need to ofload something to disk, we pass it to an async wq.
	if (RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed) < nvmeibt_tlv_get_idx(&(msg->persist_and_wire_buf.topo_ctx))) {
		N_Tf(cusnpt4, "Sending APPEND_ENTRIES_REP (TOPO, follower_committed)=@INT64_TX < incoming_messaging_buf->topo_ctx.idx=@INT64_TX", RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_committed), nvmeibt_tlv_get_idx(&(msg->persist_and_wire_buf.topo_ctx)));
	}

send_rep:
#ifndef TOMA_SIMULATOR
	nvmeibt_topology_serialize_active_topology();
#endif
	// Since I know nothing about the leader, and my topology buf is small, and already in
	//  memory, I always send it.
	if (rep_rv == RAFT_FOLLOWER_REP_RV_DEFERRED) {
		if (src_node->append_entries_rep_was_not_sent) {
			N_Tf(hh13336, "Send dummy REP");
		} else {
			src_node->append_entries_rep_was_not_sent = true;
			N_Tf(hh13337, "Don't send");
			goto out;
		}
	}
	is_sending_applied_topo = is_applied_topo_ready_and_different(&(cur_topo->buf_of_follower_wire_topo)) && (rep_rv == RAFT_FOLLOWER_REP_RV_TRUE);
	raft_send_msg_to_peer(RAFT_MSG_APPEND_ENTRIES_REP, src_node, (rep_rv != RAFT_FOLLOWER_REP_RV_FALSE), is_sending_applied_topo,
						  my_raft_global.follower_to_leader_wire_buf,
						  0,
						  is_sending_applied_topo ?
						     (persist_and_wire_buf_get_total_len(my_raft_global.follower_to_leader_wire_buf) -
							  sizeof(struct nvmeibt_persist_and_wire_buf)) :
						     0);

out:
	if ((rep_rv != RAFT_FOLLOWER_REP_RV_FALSE) && (rep_rv != RAFT_FOLLOWER_REP_RV_IGNORE)) {	// A deferred msg (since we persist), is considered a valid heartbeat from the leader
		raft_reset_election_timeout(0);   // Append_entries is a valid heartbeat
	} else {
		// It was an append_entries from an unworthy leader, if no other leader arises,
		//  then I will suggest myself when the timeout expires
	}
	NFOUT;
	return rv;
}

static bool nvmeibt_raft_is_distributed_topo_degrading(void);
static bool nvmeibt_raft_is_distributed_topo_scaling(void);

#define APPEND_ENTRIES_EXCEPTIONAL_TO_STD_RATIO 10

static void raft_upd_append_entries_rep_IIRs(const struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	int64_t						ns_since_append_entries;
	bool						is_committed_and_applied;
	struct nvmeib_iir			*all_APPEND_ENTRIES_REP_IIR;
	struct timespec				now;

	// Sample only committed and applied replies. This also captures standard "heartbeat" append_entries
	// When we send only a commit, the applied does not change
	// When we send only an applied the committed does not change
	is_committed_and_applied = (nvmeibt_tlv_get_idx(&(msg->persist_and_wire_buf.topo_ctx)) == RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit) &&
								msg->applied_TOPO_idx == RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority));
	// N_Tf(ghvfsgvuyr, "persist_and_wire_buf=@INT64_TD leader_to_commit=@INT64_TD msg->applied_TOPO_idx=@INT64_TD leader_committed_by_majority=@INT64_TD",
	//	 nvmeibt_tlv_get_idx(&(msg->persist_and_wire_buf.topo_ctx)), RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_to_commit),
	//	 msg->applied_TOPO_idx, RAFT_COMMIT_LIFECYCLE_VAL(TOPO, leader_committed_by_majority));
	getnstimeofday_boot(&now);
	ns_since_append_entries = timespec_diff_ns(now, nvmeibt_global_get_global()->last_raft_distribution_timestamp);
	// N_Tf(4vgs9l2, "ns_since_append_entries=@INT64_TD", ns_since_append_entries);
	if (ns_since_append_entries > CLIP_ROOF_IMAGINARY_REFERENCE_HEARTBEAT_TIMEOUT_NS) {
		goto out;
	}
	if (is_committed_and_applied) {		// Ignore replies that are completely off
		// Any APPEND_ENTRIES
		all_APPEND_ENTRIES_REP_IIR = &(src_node->peer_statistics.APPEND_ENTRIES_REP_IIR);
		nvmeib_iir_add_sample(all_APPEND_ENTRIES_REP_IIR, ns_since_append_entries);
		// NVMEIB_IIR_DUMP(vth9dmg, *all_APPEND_ENTRIES_REP_IIR, ns_since_append_entries);
		// APPEND ENTRIES that might take several iterations to complete. They involve a change and are more rare
		if (src_node->peer_statistics.is_awaiting_REP_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec) {
			src_node->peer_statistics.is_awaiting_REP_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec = 0;
			ns_since_append_entries = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), my_raft_global.leader_first_APPEND_ENTRIES_with_cur_committed_and_applied_topo_timespec);
			// N_Tf(bhdyd5p, "ns_since_append_entries(with a change)=@INT64_TD", ns_since_append_entries);
			if (ns_since_append_entries < CLIP_ROOF_IMAGINARY_REFERENCE_HEARTBEAT_TIMEOUT_NS) {
				// exceptional
				if (ns_since_append_entries > (nvmeib_iir_get_val(*all_APPEND_ENTRIES_REP_IIR) * APPEND_ENTRIES_EXCEPTIONAL_TO_STD_RATIO +
											   nvmeib_iir_get_standard_deviation(*all_APPEND_ENTRIES_REP_IIR) * APPEND_ENTRIES_EXCEPTIONAL_TO_STD_RATIO)) {
					nvmeib_iir_add_sample(&(src_node->peer_statistics.APPEND_ENTRIES_REP_exceptional_IIR), ns_since_append_entries);
					NVMEIB_IIR_DUMP(laicn6d, src_node->peer_statistics.APPEND_ENTRIES_REP_exceptional_IIR, ns_since_append_entries);
				}
				if (nvmeibt_raft_is_distributed_topo_degrading()) {
					nvmeib_iir_add_sample(&(src_node->peer_statistics.APPEND_ENTRIES_REP_degrading_IIR), ns_since_append_entries);
					// NVMEIB_IIR_DUMP(rvsu0nd, src_node->peer_statistics.APPEND_ENTRIES_REP_degrading_IIR, ns_since_append_entries);
				} else if (nvmeibt_raft_is_distributed_topo_scaling()) {
					nvmeib_iir_add_sample(&(src_node->peer_statistics.APPEND_ENTRIES_REP_scaling_IIR), ns_since_append_entries);
					// NVMEIB_IIR_DUMP(oakzrnt, src_node->peer_statistics.APPEND_ENTRIES_REP_scaling_IIR, ns_since_append_entries);
				} else {
					// N_Tf(4v2789s, "@STR IIRs not affected, since the topo was not degrading/scaling", nvmeibt_node_name(src_node));
				}
			}
		}
	}
out:;
}

static int raft_handle_append_entries_rep(const struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	int							rv = 0;
	struct nvmeibt_raft_member	*src_member = nvmeibt_node_get_raft_member(src_node);

	NFIN;
	if (my_raft_global.role != RAFT_ROLE_LEADER) {
		N_Tf(nhu7wn9, "Ignoring, my role is now @STR - @ROLE_INT", RAFT_ROLE_STR(), my_raft_global.role);
		goto out;
	}
	if (!src_member) {
		N_Tf(ccyake6, "Got reply from a non-raft_member, ignoring");
		goto out;
	}
	if (src_member->last_append_entries_rep_msg_num > msg->append_entries_msg_num) {
		N_Tf(y77113b, "Old msg received, ignoring. last_num=@UINT > msg_num=@UINT",
			 src_member->last_append_entries_rep_msg_num, msg->append_entries_msg_num);
		goto out;
	}
	if (!msg->is_vote_granted) {
		// We can get !is_vote_granted in APPEND_ENTRIES_REP only if we are old leader
		N_Tf(8dshek3, "raft is working with a term greater than my, I'm no longer a leader");
		raft_convert_to_follower(NULL, NULL);
		goto out;
	}
	raft_upd_append_entries_rep_IIRs(msg, src_node);
	rv = leader_process_peer_msg_data(msg, src_member);
	is_majority_voted_for_me_following_new_vote(src_member);
out:
	NFOUT;
	return rv;
}

static BOOL is_incoming_msg_valid(struct nvmeibt_big_msg *big_msg,
								 struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	BOOL						rv = 0;

	NFIN;
	// WARNINGS
	// Warn if a different software version
	if (msg->software_version != TOMA_SW_COMPATIBILITY_VER) {
		N_Wf(huu876y, "software version mismatch @SOFTWARE_VERSION!=@SOFTWARE_VERSION", msg->software_version, TOMA_SW_COMPATIBILITY_VER);
	}
	if (strncmp(msg->git_commit_id, GIT_COMMIT_ID, sizeof(msg->git_commit_id))) {
		static struct timespec	prev_printout_timespec = TIMESPEC_ZERO;
		if (nvmeibt_global_get_cur_event_start_time().tv_sec - prev_printout_timespec.tv_sec > 300) {
			N_Tf(qy763yr, "git Change-Id mismatch '@GIT_COMMIT_ID!=@GIT_COMMIT_ID'", msg->git_commit_id, GIT_COMMIT_ID);
			prev_printout_timespec = nvmeibt_global_get_cur_event_start_time();
	   }
	}
	// REJECTS
	// Not a valid raft msg
	if ((big_msg->msg_type & 0xffff0000) != NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT) {
		N_Ef(dju87i3, "msg_type=@MSG_TYPE", (unsigned int)big_msg->msg_type);
		goto out;
	}
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Tf(huwwe6q, "!nvmeibt_topology_is_HW_config_functional()");
		goto out;
	}
	// I received a msg that was destined to a different node
	if (!raft_is_my_uuid(&(msg->dst_node_id))) {
		N_Tf(djuir84, "sent_to!=my_id @UUID_LE!=@UUID_LE", &msg->dst_node_id, raft_get_my_uuid());
		goto out;
	}
	// Old msg
	if (msg->current_term < my_raft_global.last_rx_append_entries_term) {
		// Not from the protocol
		if (msg->msg_type == RAFT_MSG_APPEND_ENTRIES) {
			N_Wf(vsyhjk6, "Ignoring msg from old leader msg->current_term=@LLX < @LLX=last_rx_append_entries_term", msg->current_term, my_raft_global.last_rx_append_entries_term);
		} else {
			N_Tf(cvstkwi, "Ignoring msg msg->current_term=@LLX < @LLX=last_rx_append_entries_term", msg->current_term, my_raft_global.last_rx_append_entries_term);
		}
		goto out;
	}
	// Cannot receive a msg with the same term from a different leader
	if (msg->msg_type == RAFT_MSG_APPEND_ENTRIES) {	// The only message from the leader, verify it
		if (msg->current_term == nvmeibt_raft_get_current_term() && !raft_is_leader_uuid(&(msg->src_node_id)) && raft_do_we_have_a_leader()) {
			N_Wf(gg00kr5, "OOPS, leader's conflict @NODE_NAME!=@NODE_NAME", nvmeibt_node_name(src_node), raft_get_leader_node_name());
			goto out;
		}
	}
	// CRC and len check
	if ((msg->is_with_raft_log) &&
		(!is_persist_and_wire_buf_crc_and_len_ok(&msg->persist_and_wire_buf, big_msg->data_len - (int)sizeof(*msg))))
			goto out;

	// Leader ignores replies with the wrong term
	if (is_req_vote_rep_or_append_entries_rep(msg->msg_type) && nvmeibt_raft_is_leader()) {
		if (msg->current_term != nvmeibt_raft_get_current_term()) {
			N_Tf(dki99kk, "Ignoring msg from current_term=@RAFT_TERM cur_term=@RAFT_TERM", msg->current_term, nvmeibt_raft_get_current_term());
			goto out;
		}
	}
	rv = 1;
out:
	NFOUT;
	return rv;
}

static int dispatch_raft_msg(struct raft_msg *msg, struct nvmeibt_node *src_node)
{
	int							rv = 0;
	struct nvmeibt_raft_member	*src_member = nvmeibt_node_get_raft_member(src_node);

	NFIN;

	if (src_member) {
		src_member->toma_software_version = msg->software_version;
		if (src_member->is_ignored) {
			N_Tf(hu8723n, "raft_member=@STR is ignored", nvmeibt_raft_member_name(src_member));
			goto out;
		}
	} else {
		N_Tf(6cvwsk5, "Unknown member id=@UUID_LE src_node=@SRC_NODE. I might be new to this raft domain. Accepting.",
			 &(msg->src_node_id), nvmeibt_node_name(src_node));
	}
	switch (msg->msg_type) {
	case RAFT_MSG_REQ_VOTE:
		rv = raft_handle_req_vote(msg, src_node);
		break;
	case RAFT_MSG_REQ_VOTE_REP:
		rv = raft_handle_req_vote_rep(msg, src_node);
		break;
	case RAFT_MSG_APPEND_ENTRIES:
		// raft long messages test
		if (msg->msg_data_len > 1024) {
			struct raft_long_msg_test *appendix_ptr;

			appendix_ptr = (struct raft_long_msg_test *)(msg->persist_and_wire_buf.data + msg->msg_data_len - sizeof(struct raft_long_msg_test));
			if (appendix_ptr->appendix_signature == RAFT_LONG_MSG_TEST_SIGNATURE) {
				N_Tf(t_zz_10, "raft long message test, appendix len=@INT64", appendix_ptr->appendix_len);
				msg->msg_data_len -= appendix_ptr->appendix_len;
			}
		}

		rv = raft_handle_append_entries(msg, src_node);
		break;
	case RAFT_MSG_APPEND_ENTRIES_REP:
		rv = raft_handle_append_entries_rep(msg, src_node);
		break;
	default:
		N_Ef(error_raft_dispatch_raft_msg, "msg_type=@MSG_TYPE src_node=@SRC_NODE", msg->msg_type, nvmeibt_node_name(src_node));
		rv = -1;
		break;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibt_raft_handle_incoming_message(struct nvmeibt_big_msg *big_msg)
{
	int							rv = -1;
	struct raft_msg 			*msg = (struct raft_msg *)big_msg->data;
	int							is_converting_to_follower = 0;
	bool						is_msg_from_my_leader;
	struct nvmeibt_node			*src_node;
	uint32_t					msg_raft_hdr_crc, tmp_crc;

	NFIN;
	if (big_msg->data_len < (int)sizeof(*msg)) {
		N_Ef(u778333, "Raft msg is too short len=@INT", big_msg->data_len);
		goto out;
	}
	msg_raft_hdr_crc = msg->raft_hdr_crc;
	msg->raft_hdr_crc = 0;
	tmp_crc = crc32(0, msg, sizeof(*msg));
	msg->raft_hdr_crc = msg_raft_hdr_crc;
	if (tmp_crc != LE_SWAP32(msg_raft_hdr_crc)) {
		N_Ef(u778322, "Raft header CRC error, msg=@X calc=@X", LE_SWAP32(msg_raft_hdr_crc), tmp_crc);
		goto out;
	}
	convert_raft_msg_header_le_be(msg);

	src_node = nvmeibt_node_get_node_by_id(&(msg->src_node_id));
	if (!src_node) {
		N_Tf(tt33nhu, "Unknown node @UUID_LE", &(msg->src_node_id));
		goto out;
	}
	NRAFT_DUMP_MSG(ryy7564, msg, src_node, msg->msg_data_len);
	if (nvmeibt_global_get_global()->raft_pause_mode & RAFT_IN_PAUSED) {
		N_Tf(fgyt338, "Ignoring incoming message from node @UUID_LE, RAFT is paused", &msg->src_node_id);
		goto out;
	}
	// Always - Noticing a higher term than ever before, resets everything
	// A lower term results in a relevant reject
	// An equal term is accepted only if never voted for such term to a different candidate
	is_msg_from_my_leader = raft_is_leader_uuid(nvmeibt_node_UUID(src_node));
	if (msg->current_term > nvmeibt_raft_get_current_term()) {
		if ((msg->msg_type == RAFT_MSG_REQ_VOTE) && !IS_FORCED_REELECT(msg) && raft_do_we_have_a_stable_leader() && !is_msg_from_my_leader) {
			/*
				From https://raft.github.io/raft.pdf
				To prevent this problem, servers disregard RequestVote
				RPCs when they believe a current leader exists. Specifically, if a server receives a RequestVote RPC within
				the minimum election timeout of hearing from a current leader, it does not update its term or grant its vote.
				This does not affect normal elections, where each server
				waits at least a minimum election timeout before starting
				an election. However, it helps avoid disruptions from removed servers: if a leader is able to get heartbeats to its
				cluster, then it will not be deposed by larger term numbers.
			*/
			N_Tf(yy472zs, "We already have a leader, ignore the req_vote");
			goto out;
		}
		N_Tf(fhuwq77, "Received term=@RAFT_TERM>@RAFT_TERM", msg->current_term, nvmeibt_raft_get_current_term());
		my_raft_global.current_term = msg->current_term;
		is_converting_to_follower = 1;
		// If I'm a leader and the received APPEND_ENTRIES_REP msg has greater term - start reelection
		// after leader_heartbeat_timeout, because I want continue to be leader
		if ((msg->msg_type == RAFT_MSG_APPEND_ENTRIES_REP) && nvmeibt_raft_is_leader()) {
			my_raft_global.next_election_time = nvmeibt_global_get_cur_event_start_time();
			timespec_update_by_a_few_nsec(&(my_raft_global.next_election_time), raft_leader_heartbeat_timeout_nsec);
		}
	}
	if (is_raft_leader_msg(msg->msg_type)) {
		log_if_is_incoming_leader_persist_and_wire_buf_tlv_different_from_follower_committed(&(msg->persist_and_wire_buf));
	}
	if (is_converting_to_follower && !is_msg_from_my_leader) {
		raft_convert_to_follower(NULL, NULL);
	}
	if (!is_incoming_msg_valid(big_msg, msg, src_node)) {
		goto out;
	}
	nvmeibt_raft_upd_shutdown_term(msg->shutdown_term);
	rv = dispatch_raft_msg(msg, src_node);
out:
	NFOUT;
	return rv;
}

/*********    Init   *************/

static void init_lifecycle_ctx(struct raft_commit_lifecycle_ctx *l)
{
	l->leader_calculated = nvmeibt_offset_and_idx_uninitialized;
	l->leader_to_commit = nvmeibt_offset_and_idx_uninitialized;
	l->leader_committed_by_majority = nvmeibt_offset_and_idx_uninitialized;
	l->follower_submitted = nvmeibt_offset_and_idx_uninitialized;
	l->follower_committed = nvmeibt_offset_and_idx_uninitialized;
	l->follower_applied = nvmeibt_offset_and_idx_uninitialized;
	l->follower_to_apply = nvmeibt_offset_and_idx_uninitialized;
	l->follower_sent_to_leader = nvmeibt_offset_and_idx_uninitialized;
	l->leader_n_peers_committed = 0;
}

void nvmeibt_raft_activate(void)
{
	raft_reset_election_timeout(0);
	nvmeibt_toma_raft_validity_was_updated();
}

int nvmeibt_raft_one_time_init(void)
{
	int							rv = 0;

	NFIN;
	my_raft_global.raft_members_hash_by_uuid = NVMEIB_HASH_CREATE(a7y2k49, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "raft_members_hash", 16, 0);
	my_raft_global.n_raft_members = 0;
	nvmeibt_raft_recalc_timeout_constants();
	getnstimeofday_boot(&(my_raft_global.next_election_time));
	my_raft_global.last_recieved_append_entries_timestamp_sec = my_raft_global.next_election_time.tv_sec;	// init with "now"
	init_lifecycle_ctx(&(my_raft_global.TOPO_commit_lifecycle));
	init_lifecycle_ctx(&(my_raft_global.TOPO_CONFIG_commit_lifecycle));
	init_lifecycle_ctx(&(my_raft_global.KAFKA_MGMT_CONFIG_commit_lifecycle));
	init_lifecycle_ctx(&(my_raft_global.RAFT_MEMBERS_commit_lifecycle));
	init_lifecycle_ctx(&(my_raft_global.RAFT_MEMBERS_SEQ_NO_commit_lifecycle));
	init_lifecycle_ctx(&(my_raft_global.current_raft_TERM_commit_lifecycle));
	my_raft_global.shutdown_term = 0;
	my_raft_global.leader_shutdown_term = 0;
	my_raft_global.first_append_entries_term = 0;
	my_raft_global.last_rx_append_entries_term = 0;
	my_raft_global.last_rx_append_entries_msg_num = 0;
	my_raft_global.last_tx_append_entries_msg_num = 0;
	nvmeibt_raft_set_voted_for_uuid(NULL);
	nvmeibt_raft_set_leader_uuid(NULL);
	write_leader_name_to_file("");
	leader_reset_peers_committed_values();
	raft_leader_reset_counters_upon_last_LOG_change();
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_topo_complete));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_topo_config_complete));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_kafka_mgmt_config_complete));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_raft_members_complete));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_topo_incremental));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_topo_config_incremental));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_kafka_mgmt_config_incremental));
	NVMEIBT_BUF_INIT(&(my_raft_global.leader_to_commit_wire_raft_members_incremental));
	if (nvmeibt_recursive_mkdir(NVMEIBT_PERSISTENCY_CACHE_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0) {
		rv = -1;
		goto out;
	}
	nvmeibt_topology_active_mark_reserialization_required();	// At least the initial one
	//
	{ _Static_assert((sizeof(struct raft_persistency) / 8) * 8 == sizeof(struct raft_persistency), "sizeof(struct raft_persistency) is not a multiple of 8"); }
	{ _Static_assert((sizeof(struct nvmeibt_wire_type_len_value) / 8) * 8 == sizeof(struct nvmeibt_wire_type_len_value), "sizeof(struct nvmeibt_wire_type_len_value) is not a multiple of 8"); }
	{ _Static_assert((sizeof(struct nvmeibt_persist_and_wire_buf) / 8) * 8 == sizeof(struct nvmeibt_persist_and_wire_buf), "sizeof(struct nvmeibt_persist_and_wire_buf) is not a multiple of 8"); }

out:
	NFOUT;
	return rv;
}

void nvmeibt_raft_set_discard_append_entries(int discard_num, bool is_permanent)
{
	N_Tf(x_tt_42, "RAFT discard @INT messages on changes, permanent=@BOOL", discard_num, is_permanent);
	raft_num_of_messages_to_discard_after_change = discard_num;
	raft_num_of_messages_to_discard_after_change_tmp = discard_num + 1;
	is_discard_after_change_permanent = is_permanent;
}

void convert_raft_msg_header_le_be(struct raft_msg *msg)
{
	{ _Static_assert(sizeof(struct raft_msg) == 384, "Struct mm_segment_conf was changed without updating the packing function!"); }

	SWAP32_STR_FIELD(msg, software_version);
	// git_commit_id is a string. No need to touck
	SWAP32_STR_BITFIELD(msg, msg_type);
	SWAP_UUID_STR_FIELD(msg, src_node_id);
	SWAP32_STR_FIELD(msg, src_node_idx);
	SWAP_UUID_STR_FIELD(msg, dst_node_id);
	SWAP32_STR_FIELD(msg, dst_node_idx);
	SWAP64_STR_FIELD(msg, current_term);
	SWAP64_STR_FIELD(msg, shutdown_term);
	SWAP64_STR_FIELD(msg, applied_TOPO_idx);
	SWAP64_STR_FIELD(msg, applied_TOPO_CONFIG_idx);
	SWAP64_STR_FIELD(msg, applied_raft_members_offset);
	SWAP64_STR_FIELD(msg, local_serialization_version);
	SWAP32_STR_FIELD(msg, append_entries_msg_num);
	SWAP8_STR_FIELD(msg, is_vote_granted);
	SWAP8_STR_FIELD(msg, is_with_raft_log);
	SWAP32_STR_FIELD(msg, msg_data_len);
	//SWAP32_STR_FIELD(msg, raft_hdr_crc);
}

/*****************     IIR based calculation of timeouts    *******************/

// The objective is to calculate the leader heartbeat timeout and the factor
// The actual (effective) calculation is done by the followers
//
// Samples:
// - Every node calculated the ping avg time and sd (over all nodes over IIR-based samples)
// - The leader calculates the APPEND_ENTRIES_with_data_rep avg time and sd (over all TOMAs over IIR-based samples)
// - The leader calculates the topo_calc time (IIR)
// The leader distributes the append_entries_rep and topo_calc values in the persist_and_wire_buf
// The follower calculates the effective time and factor based on its ping and received values
// If the follower is disconnected (no leader) it increases the timeout based on ping responsiveness

#include "../common/nvmeib_math.h"
#include "../common/nvmeib_iir.h"

#define N_SDs_ADDED_ON_TOP_OF_AVG 1/2

#define NVMEIB_BASIC_STATISTICS_DUMP(name, _stats_dump) ({																				\
	struct nvmeib_basic_statistics	*stats_dump = (_stats_dump);																		\
	N_Tf(name, "@STR n=@INT avg=@INT64_TDns sd=@INT64_TD", 																				\
		 nvmeib_basic_statistics_get_desc(stats_dump), nvmeib_basic_statistics_get_n(stats_dump),										\
		 (int64_t)nvmeib_basic_statistics_get_avg(stats_dump), (int64_t)nvmeib_basic_statistics_get_standard_deviation(stats_dump));	\
})

#define FACTOR_OF_EFFECTIVE_PING_TIMEOUT_OVER_CALCULATED 2
#define FACTOR_OF_EFFECTIVE_LEADER_TIMEOUT_OVER_CALCULATED 3/2
#define RATIO_BETWEEN_NO_PING_TIME_AND_TIMEOUT 100

int64_t nvmeibt_raft_get_roof_leader_heartbeat_timeout_ns(void)
{
	return CLIP_ROOF_LEADER_HEARTBEAT_TIMEOUT_NS;
}

int64_t nvmeibt_raft_get_persistent_leader_append_entries_time_ns(void)
{
	return persist_and_wire_buf_get_raft_calculated_leader_append_entries_rep_time_ns(my_raft_global.follower_to_commit_persist_and_wire_buf_full);
}

int64_t nvmeibt_raft_get_persistent_leader_topo_calc_time_ns(void)
{
	return persist_and_wire_buf_get_raft_calculated_leader_topo_calc_time_ns(my_raft_global.follower_to_commit_persist_and_wire_buf_full);
}

int64_t nvmeibt_raft_leader_get_append_entries_rep_time_ns_for_persist_and_wire_buf(void)
{
	return (nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns ?
			nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns : nvmeibt_raft_get_persistent_leader_append_entries_time_ns());
}

void nvmeibt_raft_leader_set_calculated_append_entries_rep_time_ns_USED_ONLY_BY_JSON_PARSER(int64_t calculated_append_entries_rep_time_ns)
{
	nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns = calculated_append_entries_rep_time_ns;
	N_Tf(nzt2b6c, "calculated_append_entries_rep_time_ns=@LLD", nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns);
}

int64_t nvmeibt_raft_leader_get_topo_calc_time_ns_for_persist_and_wire_buf(void)
{
	return (nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns ?
			nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns : nvmeibt_raft_get_persistent_leader_topo_calc_time_ns());
}

void nvmeibt_raft_leader_set_calculated_topo_calc_time_ns_USED_ONLY_BY_JSON_PARSER(int64_t calculated_topo_calc_time_ns)
{
	nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns = calculated_topo_calc_time_ns;
	N_Tf(gna92kr, "calculated_topo_calc_time_ns=@LLD", nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns);
}

static bool nvmeibt_raft_is_distributed_topo_scaling(void)
{
	// We have to learn when praids are added in order to respond to scale
	return (nvmeibt_global_adaptive_timeouts()->n_praids.new_in_topo > 0);
}

static bool nvmeibt_raft_is_distributed_topo_degrading(void)
{
	// We have to learn when praids are added in order to respond to scale
	return (nvmeibt_global_adaptive_timeouts()->n_praids.became_not_activated > 0 ||
			nvmeibt_global_adaptive_timeouts()->n_praids.with_new_dead > 0);
}

void nvmeibt_raft_follower_upd_effective_raft_heartbeat_timeout_and_factor(void)
{
	int64_t					leader_append_entries_time_ns;
	int64_t					leader_topo_calc_time_ns;
	//
	int64_t					effective_timeout_based_on_ping_ns;
	int64_t					effective_timeout_based_on_leader_ns;
	int64_t					effective_timeout_baseline_ns;
	static int64_t			prev_effective_timeout_baseline_ns;
	//
	double					factor_of_baseline_timeout_over_default;
	static double			prev_factor_of_baseline_timeout_over_default;
	int64_t					effective_timeout_ns;
	static int64_t			prev_effective_factor;
	int						effective_factor;
	static int				prev_effective_timeout_ns;
	//
	int64_t					ns_since_last_ping_calc;
	int64_t					ns_since_boot;

	// NFIN;
	// Take into account disconnects and the persistent values
	leader_append_entries_time_ns = nvmeibt_raft_get_persistent_leader_append_entries_time_ns();
	leader_topo_calc_time_ns = nvmeibt_raft_get_persistent_leader_topo_calc_time_ns();
	// N_Tf(gvghcsvc, "leader_append_entries_time_ns=@INT64_TD leader_topo_calc_time_ns=@INT64_TD", leader_append_entries_time_ns, leader_topo_calc_time_ns);
	//
	effective_timeout_based_on_leader_ns = (leader_append_entries_time_ns + leader_topo_calc_time_ns) * FACTOR_OF_EFFECTIVE_LEADER_TIMEOUT_OVER_CALCULATED;
	effective_timeout_baseline_ns = min(CLIP_ROOF_IMAGINARY_REFERENCE_HEARTBEAT_TIMEOUT_NS, max(CLIP_FLOOR_LEADER_HEARTBEAT_TIMEOUT_NS, effective_timeout_based_on_leader_ns));
	if (NSEC_TO_MSEC(effective_timeout_baseline_ns) != NSEC_TO_MSEC(prev_effective_timeout_baseline_ns)) {
		N_Tf(3vyonr6, "effective_timeout_baseline=@INTms leader=@INTms", (int)NSEC_TO_MSEC(effective_timeout_baseline_ns), (int)NSEC_TO_MSEC(effective_timeout_based_on_leader_ns));
		prev_effective_timeout_baseline_ns = effective_timeout_baseline_ns;
	}
	if (!raft_do_we_have_a_leader()) {
		// Disconnected
		effective_timeout_based_on_ping_ns = nvmeibt_global_adaptive_timeouts()->follower_calculated_timeout_based_on_ping_ns * FACTOR_OF_EFFECTIVE_PING_TIMEOUT_OVER_CALCULATED;	// Mine
		effective_timeout_baseline_ns = max(effective_timeout_baseline_ns, effective_timeout_based_on_ping_ns);
		ns_since_boot = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_get_startup_timespec());
		ns_since_last_ping_calc = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_adaptive_timeouts()->timespec_of_last_successful_calc_based_on_ping_response);
		N_Tf(m4xfjep, "based_on_ping=@INTms ns_since_boot=@INT64_TD ns_since_last_ping_calc=@INT64_TD",
			 (int)NSEC_TO_MSEC(effective_timeout_based_on_ping_ns), ns_since_boot, ns_since_last_ping_calc);
		effective_timeout_baseline_ns = max(effective_timeout_baseline_ns, min(ns_since_boot, ns_since_last_ping_calc) / RATIO_BETWEEN_NO_PING_TIME_AND_TIMEOUT);
	}
	//	Split the excess factor between the HEARTBEAT_TIMEOUT and the MIN_ELECTION_TIMEOUT_FACTOR. Each gets a "sqrt()" of it
	factor_of_baseline_timeout_over_default = (double)effective_timeout_baseline_ns / RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT;
	effective_factor = sqrt(factor_of_baseline_timeout_over_default) * RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT;
	effective_timeout_ns = effective_timeout_baseline_ns * RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT / effective_factor;
	if (factor_of_baseline_timeout_over_default != prev_factor_of_baseline_timeout_over_default || effective_factor != prev_effective_factor || effective_timeout_ns != prev_effective_timeout_ns) {
		N_Tf(vgduk35, "factor_of_baseline_timeout_over_default=@INT64_TD/1000 effective_timeout=@INTms effective_factor=@INT",
			 (int64_t)(factor_of_baseline_timeout_over_default * 1000), (int)NSEC_TO_MSEC(effective_timeout_ns), effective_factor);
		prev_factor_of_baseline_timeout_over_default = factor_of_baseline_timeout_over_default;
		prev_effective_factor = effective_factor;
		prev_effective_timeout_ns = effective_timeout_ns;
	}
	// Clip
	effective_timeout_ns = min(CLIP_ROOF_LEADER_HEARTBEAT_TIMEOUT_NS, max(CLIP_FLOOR_LEADER_HEARTBEAT_TIMEOUT_NS, effective_timeout_ns));
	effective_factor = min(CLIP_ROOF_LEADER_ELECTION_TIMEOUT_FACTOR, max(CLIP_FLOOR_LEADER_ELECTION_TIMEOUT_FACTOR, effective_factor));
	//
	if (effective_timeout_ns != prev_effective_timeout_ns || effective_factor != prev_effective_factor) {
		N_Tf(njrioe5, "Effective clipped timeouts: total=@INTms factor=@INT", (int)NSEC_TO_MSEC(effective_timeout_ns), effective_factor);
	}
	// Update raft with the new values
	if (1) {
		_DILUTED_CMD(30000, N_Tf(vyf095l, "!For now, not updating the real effective values!"));
	} else {
		if (	(nvmeibt_raft_get_effective_heartbeat_timeout_ns() != effective_timeout_ns ||
				 nvmeibt_raft_get_effective_min_election_timeout_factor() != effective_factor)) {
			nvmeibt_raft_set_effective_leader_heartbeat_timeout_ns(effective_timeout_ns, 0);
			nvmeibt_raft_set_effective_min_election_timeout_factor(effective_factor, 0);
			N_Tf(wnz7voe, "raft_leader_heartbeat_timeout_nsec=@INT64_TD raft_min_election_timeout_factor=@INT64_TD",
				 nvmeibt_raft_get_effective_heartbeat_timeout_ns(), nvmeibt_raft_get_effective_min_election_timeout_factor());
			nvmeibt_raft_recalc_timeout_constants();
		}
	}
	// NFOUT;
}

#define stats_to_val(__stats)		\
	((int64_t)(nvmeib_basic_statistics_get_avg(&__stats) + (nvmeib_basic_statistics_get_standard_deviation(&__stats) * N_SDs_ADDED_ON_TOP_OF_AVG)))
static inline void max_me_with(int64_t *me, int64_t with) { *me = max(*me, with); }

static struct nvmeib_basic_statistics		ping_stats;
static struct nvmeib_basic_statistics		ping_exceptional_stats;
static struct nvmeib_basic_statistics		append_entries_stats;
static struct nvmeib_basic_statistics		append_entries_exceptional_stats;
// Calc timeouts based on responsiveness of ping and append_entries_with_data
// Also take into account the leader's topo_calc time
// Note that the calculations are based on replies, and not on the lack of replied,
//  hence it is quite resilient to non-responding nodes and to quiet periods
//  (E.g., no append_entries with data for a month).
// Nevertheless, we somewhat filter-out non-responsive nodes
void nvmeibt_raft_calc_timeouts_based_on_IIRs(void)
{
	struct nvmeibt_node						*node;
	int										n_nodes = 0;
	//
	int64_t									iir_ping_time_ns;
	double									iir_ping_saturation;
	double									iir_ping_sd;
	//
	int64_t									iir_ping_exceptional_time_ns;
	double									iir_ping_exceptional_saturation;
	double									iir_ping_exceptional_sd;
	//
	int64_t									iir_append_entries_time_ns;
	double									iir_append_entries_saturation;
	double									iir_append_entries_sd;
	bool									is_enough_append_entries_stats;
	//
	int64_t									iir_append_entries_exceptional_time_ns;
	double									iir_append_entries_exceptional_saturation;
	double									iir_append_entries_exceptional_sd;
	static struct nvmeib_basic_statistics	prev_append_entries_exceptional_stats;
	bool									is_enough_append_entries_exceptional_stats;
	//
	int64_t									iir_append_entries_scaling_time_ns;
	double									iir_append_entries_scaling_saturation;
	double									iir_append_entries_scaling_sd;
	struct nvmeib_basic_statistics			append_entries_scaling_stats;
	static struct nvmeib_basic_statistics	prev_append_entries_scaling_stats;
	bool									is_enough_append_entries_scaling_stats;
	//
	int64_t									iir_append_entries_degrading_time_ns;
	double									iir_append_entries_degrading_saturation;
	double									iir_append_entries_degrading_sd;
	struct nvmeib_basic_statistics			append_entries_degrading_stats;
	static struct nvmeib_basic_statistics	prev_append_entries_degrading_stats;
	bool									is_enough_append_entries_degrading_stats;
	//
	struct nvmeib_iir						*iir;
	static double							prev_append_entries_exceptional_avg;

	// NFIN;
	nvmeib_basic_statistics_reset(&ping_stats, "PING");
	nvmeib_basic_statistics_reset(&ping_exceptional_stats, "PING_EXCEPTIONAL");
	nvmeib_basic_statistics_reset(&append_entries_stats, "APPEND_ENTRIES");
	nvmeib_basic_statistics_reset(&append_entries_scaling_stats, "APPEND_ENTRIES_scaling");
	nvmeib_basic_statistics_reset(&append_entries_degrading_stats, "APPEND_ENTRIES_degrading");
	nvmeib_basic_statistics_reset(&append_entries_exceptional_stats, "APPEND_ENTRIES_exceptional");
	NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {  // Go over all the nodes (except for me)
		n_nodes++;
		if (nvmeibt_node_is_my_node(node)) {	// Exclude myself. Both the ping and the append_entries should be shorter for self
			continue;
		}
		// PING response. Ping is periodic, regardless of leader/follower
		iir = &(node->peer_statistics.ping_response_time_IIR);
		iir_ping_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_ping_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_ping_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_ping_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&ping_stats, iir_ping_time_ns + (iir_ping_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
		}
		iir = &(node->peer_statistics.ping_response_time_exceptional_IIR);
		iir_ping_exceptional_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_ping_exceptional_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_ping_exceptional_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_ping_exceptional_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&ping_exceptional_stats, iir_ping_exceptional_time_ns + (iir_ping_exceptional_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
		}
		if (!nvmeibt_raft_is_leader()) {
			continue;
		}
		// APPEND_ENTRIES with data REP. It might be that we didn't encounter any for quite some time. We use the last ones
		iir = &(node->peer_statistics.APPEND_ENTRIES_REP_IIR);
		iir_append_entries_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_append_entries_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_append_entries_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_append_entries_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&append_entries_stats, iir_append_entries_time_ns + (iir_append_entries_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
		}
		iir = &(node->peer_statistics.APPEND_ENTRIES_REP_exceptional_IIR);
		iir_append_entries_exceptional_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_append_entries_exceptional_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_append_entries_exceptional_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_append_entries_exceptional_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&append_entries_exceptional_stats, iir_append_entries_exceptional_time_ns + (iir_append_entries_exceptional_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
			//
			if (prev_append_entries_exceptional_avg != nvmeib_basic_statistics_get_avg(&append_entries_exceptional_stats)) {
				prev_append_entries_exceptional_avg = nvmeib_basic_statistics_get_avg(&append_entries_exceptional_stats);
				N_Tf(fcdtwgh, "@STR append_entries_exceptional time=@INT64_TD saturation=@DOUBLE sd=@DOUBLE", nvmeibt_node_name(node),
					 iir_append_entries_exceptional_time_ns, iir_append_entries_exceptional_saturation, iir_append_entries_exceptional_sd);
				NVMEIB_BASIC_STATISTICS_DUMP(tvsytgvshgv, &append_entries_exceptional_stats);
			}
		}
		iir = &(node->peer_statistics.APPEND_ENTRIES_REP_scaling_IIR);
		iir_append_entries_scaling_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_append_entries_scaling_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_append_entries_scaling_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_append_entries_scaling_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&append_entries_scaling_stats, iir_append_entries_scaling_time_ns + (iir_append_entries_scaling_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
		}
		iir = &(node->peer_statistics.APPEND_ENTRIES_REP_degrading_IIR);
		iir_append_entries_degrading_time_ns = (int64_t)(nvmeib_iir_get_val(*iir));
		iir_append_entries_degrading_saturation = nvmeib_iir_get_saturation_level(*iir);
		iir_append_entries_degrading_sd = nvmeib_iir_get_standard_deviation(*iir);
		if (iir_append_entries_degrading_saturation > 0.1) {
			nvmeib_basic_statistics_add_val(&append_entries_degrading_stats, iir_append_entries_degrading_time_ns + (iir_append_entries_degrading_sd * N_SDs_ADDED_ON_TOP_OF_AVG));
		}
	}
	// PING, First try exceptional
	_DILUTED_CMD(15000, NVMEIB_BASIC_STATISTICS_DUMP(tbs0sm3, &ping_exceptional_stats));
	_DILUTED_CMD(10000, NVMEIB_BASIC_STATISTICS_DUMP(3yh9sjw, &ping_stats));
	if (nvmeib_basic_statistics_get_n(&ping_exceptional_stats) + 1 >= (n_nodes * 2 / 3)) {	// Not enough connected (+1 since excluded self)
		nvmeibt_global_adaptive_timeouts()->timespec_of_last_successful_calc_based_on_ping_response = nvmeibt_global_get_cur_event_start_time();
		nvmeibt_global_adaptive_timeouts()->follower_calculated_timeout_based_on_ping_ns = stats_to_val(ping_exceptional_stats);
	} else {
		if (nvmeib_basic_statistics_get_n(&ping_stats) + 1 < (n_nodes * 2 / 3)) {	// Not enough connected (+1 since excluded self)
			N_Tf(buen6b1, "No recalc of ping stats @INT/@INT nodes", nvmeib_basic_statistics_get_n(&ping_stats), n_nodes);
		} else {
			nvmeibt_global_adaptive_timeouts()->timespec_of_last_successful_calc_based_on_ping_response = nvmeibt_global_get_cur_event_start_time();
			nvmeibt_global_adaptive_timeouts()->follower_calculated_timeout_based_on_ping_ns = stats_to_val(ping_stats);
		}
	}
	if (!nvmeibt_raft_is_leader()) {
		nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns = 0;	// A non-leader should not use it
		goto out;
	}
	// LEADER's calculations
	iir = &(nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns_IIR);
	nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns = (nvmeib_iir_get_saturation_level(*iir) > 0.1 ? 	// Not based on peers. An exception in this function
																			   nvmeib_iir_get_val(*iir) + (nvmeib_iir_get_standard_deviation(*iir) * N_SDs_ADDED_ON_TOP_OF_AVG) : 0);
	if (append_entries_exceptional_stats.sum != prev_append_entries_exceptional_stats.sum) {
		NVMEIB_BASIC_STATISTICS_DUMP(vhdk03l, &append_entries_exceptional_stats);
		prev_append_entries_exceptional_stats = append_entries_exceptional_stats;
	}
	if (append_entries_scaling_stats.sum != prev_append_entries_scaling_stats.sum) {
		NVMEIB_BASIC_STATISTICS_DUMP(ixnjy3l, &append_entries_scaling_stats);
		prev_append_entries_scaling_stats = append_entries_scaling_stats;
	}
	if (append_entries_degrading_stats.sum != prev_append_entries_degrading_stats.sum) {
		NVMEIB_BASIC_STATISTICS_DUMP(2qnyv7f, &append_entries_degrading_stats);
		prev_append_entries_degrading_stats = append_entries_degrading_stats;
	}
	//
	is_enough_append_entries_stats = (nvmeib_basic_statistics_get_n(&append_entries_stats) + 1 >= (n_nodes * 2 / 3));
	is_enough_append_entries_exceptional_stats = (nvmeib_basic_statistics_get_n(&append_entries_exceptional_stats) + 1 >= (n_nodes * 2 / 3));
	is_enough_append_entries_scaling_stats = (nvmeib_basic_statistics_get_n(&append_entries_scaling_stats) + 1 >= (n_nodes * 2 / 3));
	is_enough_append_entries_degrading_stats = (nvmeib_basic_statistics_get_n(&append_entries_degrading_stats) + 1 >= (n_nodes * 2 / 3));
	if (is_enough_append_entries_exceptional_stats || (is_enough_append_entries_scaling_stats && is_enough_append_entries_degrading_stats)) {
		// Only when we have enough statistics on both measures we can forget the history and start from scratch
		// If one of them is missing then it might be that the current value was calculated on a different leader using the other measure
		nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns = 0;
	} else {	// If not "interesting" data, then use the ordinary append_entries, but do not overide the history (use max)
		if (is_enough_append_entries_stats) {      // Enough TOMAs replied since I became a leader
			max_me_with(&nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns, stats_to_val(append_entries_stats));
		} else {
			_DILUTED_CMD(10000, N_Tf(rtvhys2, "No recalc of append_entries_ALL_stats from @INT/@INT nodes", nvmeib_basic_statistics_get_n(&append_entries_stats), n_nodes));
		}
	}
	if (is_enough_append_entries_exceptional_stats) {		// Enough TOMAs replied since I became a leader
		max_me_with(&nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns, stats_to_val(append_entries_exceptional_stats));
	} else {
		_DILUTED_CMD(10000, N_Tf(wiu0n1z, "No recalc of append_entries_exceptional_stats from @INT/@INT nodes", nvmeib_basic_statistics_get_n(&append_entries_exceptional_stats), n_nodes));
	}
	if (is_enough_append_entries_scaling_stats) {		// Enough TOMAs replied since I became a leader
		max_me_with(&nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns, stats_to_val(append_entries_scaling_stats));
	} else {
		_DILUTED_CMD(25000, N_Tf(4hditn6, "No recalc of append_entries_scaling stats from @INT/@INT nodes", nvmeib_basic_statistics_get_n(&append_entries_scaling_stats), n_nodes));
	}
	if (is_enough_append_entries_degrading_stats) {      // Enough TOMAs replied since I became a leader
		max_me_with(&nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns, stats_to_val(append_entries_degrading_stats));
	} else {
		_DILUTED_CMD(30000, N_Tf(vhs893k, "No recalc of append_entries_degrading stats from @INT/@INT nodes", nvmeib_basic_statistics_get_n(&append_entries_degrading_stats), n_nodes));
	}
out:
	;//NFOUT;
}

void nvmeibt_raft_reset_leader_calculated_IIRs(void)
{
	struct nvmeibt_node		*node;

	NFIN;
	NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {
		// Re learn the append_entries stats. It might be that many volumes were added since the last time I was a leader
		nvmeibt_node_reset_IIRs(node, 0);
	}
	NVMEIB_IIR_RESET(yw73ncp, &(nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns_IIR), NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT, "topo_calc_time", "");
	// Until we have well established values, use the ones from persistence (by zeroing them to uninitialized)
	nvmeibt_global_adaptive_timeouts()->leader_calculated_append_entries_rep_time_ns = 0;
	nvmeibt_global_adaptive_timeouts()->leader_calculated_topo_calc_time_ns = 0;
	NFOUT;
}

int nvmeibt_raft_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_raft_member		*peer_member;
	struct timespec					elapsed;
	__kernel_long_t					now_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;
	int								i;

	NFIN;
	(*printf_fn)(printf_ctx, "RAFT\n");
	(*printf_fn)(printf_ctx, "\t- term=%llx\n", nvmeibt_raft_get_current_term());
	(*printf_fn)(printf_ctx, "\t- PING time: avg=%lldus exceptional=%lldus(n=%d)\n",
				 NSEC_TO_USEC((int64_t)nvmeib_basic_statistics_get_avg(&ping_stats)), NSEC_TO_USEC((int64_t)nvmeib_basic_statistics_get_avg(&ping_exceptional_stats)), nvmeib_basic_statistics_get_n(&ping_exceptional_stats));
	if (my_raft_global.role == RAFT_ROLE_LEADER) {
		(*printf_fn)(printf_ctx, "\t- Leader APPEND_ENTRIES time: avg=%lldus exceptional=%lldus(n=%d)\n", NSEC_TO_USEC((int64_t)nvmeib_basic_statistics_get_avg(&append_entries_stats)),
					 NSEC_TO_USEC((int64_t)nvmeib_basic_statistics_get_avg(&append_entries_exceptional_stats)), nvmeib_basic_statistics_get_n(&append_entries_exceptional_stats));
	}
	(*printf_fn)(printf_ctx, "\t- persistent leader: append_entries_time=%lldus topo_calc=%lldus\n",
				 NSEC_TO_USEC(nvmeibt_raft_get_persistent_leader_append_entries_time_ns()), NSEC_TO_USEC(nvmeibt_raft_get_persistent_leader_topo_calc_time_ns()));
	(*printf_fn)(printf_ctx, "\t- effective: timeout=%lldms factor=%lld\n",
				 NSEC_TO_MSEC(nvmeibt_raft_get_effective_heartbeat_timeout_ns()), nvmeibt_raft_get_effective_min_election_timeout_factor());
	(*printf_fn)(printf_ctx, "\t- n_peers_voted_for_me=%d\n", my_raft_global.n_peers_voted_for_me);
	(*printf_fn)(printf_ctx, "\t- leader history (secs_ago)\n");
	for (i = n_changes_in_leader_history_array - 1; i >= max(0, n_changes_in_leader_history_array - ARRAY_SIZE(leader_history_array)); --i) {
		int entry_idx = i % ARRAY_SIZE(leader_history_array);
		(*printf_fn)(printf_ctx, "\t\t- %s %lld\n",
					 nvmeibt_node_name(nvmeibt_node_get_node_by_id(&(leader_history_array[entry_idx].leader_uuid))), max(0, now_sec - leader_history_array[entry_idx].timestamp_sec));
	}
	if (my_raft_global.role == RAFT_ROLE_UNKNOWN) {			// Handle annoying race conditions of various components printing status before rat wa initialized (during toma boot) which causes a crash without logs
		(*printf_fn)(printf_ctx, "\t- Uninitialized yet!\n");
	} else if (my_raft_global.role != RAFT_ROLE_FOLLOWER) {
		if (my_raft_global.role == RAFT_ROLE_LEADER) {
			(*printf_fn)(printf_ctx, "\t- Status=LEADER\tMature=%s\n", (my_raft_global.is_leader_mature ? "Yes" : "No"));
			elapsed = timespec_sub(nvmeibt_global_get_cur_event_start_time(), my_raft_global.time_converted_to_leader);
			(*printf_fn)(printf_ctx, "\t\t- time_since_converted_to_leader=%lld.%09lld\n",
					elapsed.tv_sec, elapsed.tv_nsec);
		} else {
			(*printf_fn)(printf_ctx, "\t- Status=CANDIDATE\n");
		}
		(*printf_fn)(printf_ctx, "\t- Peer members\n");
		NVMEIB_HASH_FOREACH(peer_member, my_raft_global.raft_members_hash_by_uuid) {
			elapsed = timespec_sub(nvmeibt_global_get_cur_event_start_time(), peer_member->last_received_voted_for_me_timespec);
			(*printf_fn)(printf_ctx, "\t\t- %s: time_since_voted_for_me_on_cur_term=%lld.%09lld is_alive_for_topo_msec=%lld committed(topo_version=%llx kafka_offset=%lld)\n", nvmeibt_raft_member_name(peer_member),
						 elapsed.tv_sec, elapsed.tv_nsec, timespec_diff_ms(nvmeibt_global_get_cur_event_start_time(), peer_member->is_alive_for_topo_start_timespec),
						 (nvmeibt_raft_is_leader() ? nvmeibt_tlv_get_idx(&(peer_member->committed_persist_and_wire_buf_hdr.topo_ctx)) : -1LL),
						 (nvmeibt_raft_is_leader() ? nvmeibt_tlv_get_idx(&(peer_member->committed_persist_and_wire_buf_hdr.kafka_mgmt_config_ctx)) : nvmeibt_offset_and_idx_uninitialized));
		}
	} else {
		(*printf_fn)(printf_ctx, "\t- Status=%s, LEADER=%s\n", RAFT_ROLE_STR(), raft_get_leader_node_name());
	}
	NFOUT;
	return 0;
}

int nvmeibt_raft_print_status_json(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_raft_member		*peer_member;
	struct timespec					elapsed;
	int								n_members, i;

	NFIN;
	(*printf_fn)(printf_ctx, "\"RAFT\": {\"leader_name\": \"%s\", \"n_peers_voted_for_me\": %d, \"Role\":",
				 raft_get_leader_node_name(), my_raft_global.n_peers_voted_for_me);
	if (my_raft_global.role != RAFT_ROLE_FOLLOWER) {
		if (my_raft_global.role == RAFT_ROLE_LEADER) {
			(*printf_fn)(printf_ctx, "\"leader\", \"mature\": \"%s\", ", (my_raft_global.is_leader_mature ? "Yes" : "No"));
			elapsed = timespec_sub(nvmeibt_global_get_cur_event_start_time(), my_raft_global.time_converted_to_leader);
			(*printf_fn)(printf_ctx, "\"time_since_converted_to_leader\": %lld.%09lld, ",elapsed.tv_sec, elapsed.tv_nsec);
		} else {
			(*printf_fn)(printf_ctx, "\"candidate\", ");
		}
		n_members = nvmeib_hash_get_n_elements(my_raft_global.raft_members_hash_by_uuid);
		i = 1;
		(*printf_fn)(printf_ctx, "\"peer_members\": [");
		NVMEIB_HASH_FOREACH(peer_member, my_raft_global.raft_members_hash_by_uuid) {
			elapsed = timespec_sub(nvmeibt_global_get_cur_event_start_time(), peer_member->last_received_voted_for_me_timespec);
			(*printf_fn)(printf_ctx, "{\"member\": \"%s\", \"time_since_voted_for_me_on_cur_term\": %lld.%09lld, \"is_alive_for_topo\": %d, "
						             "\"topo_version\": %llu, \"kafka_offset\": %lld}",
						 nvmeibt_raft_member_name(peer_member),
						 elapsed.tv_sec, elapsed.tv_nsec, peer_member->is_alive_for_topo,
						 (nvmeibt_raft_is_leader() ? nvmeibt_tlv_get_idx(&(peer_member->committed_persist_and_wire_buf_hdr.topo_ctx)) : -1LL),
						 (nvmeibt_raft_is_leader() ? nvmeibt_tlv_get_idx(&(peer_member->committed_persist_and_wire_buf_hdr.kafka_mgmt_config_ctx)) : nvmeibt_offset_and_idx_uninitialized));
			if (i < n_members) {
				(*printf_fn)(printf_ctx, ", ");
				i++;
			}
		}
		(*printf_fn)(printf_ctx, "]");
	} else {
		(*printf_fn)(printf_ctx, "\"follower\"");
	}
	(*printf_fn)(printf_ctx, "}\n");

	NFOUT;
	return 0;
}

int nvmeibt_leader_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	NFIN;
	(*printf_fn)(printf_ctx, "%s\n", raft_get_leader_node_name());
	NFOUT;
	return 0;
}

