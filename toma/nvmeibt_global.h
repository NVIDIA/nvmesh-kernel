#ifndef NVMEIBT_GLOBAL
#define NVMEIBT_GLOBAL

#include <time.h>
#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "../common/nvmeib_shared.h"
#include "nvmeibt_seg_active.h"
#include "clnt/nvmeibt_client.h"

#include "nvmeibt_disk_segment_basics.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_nic.h"
#include "nvmeibt_disk.h"
#include "nvmeibt_node.h"
#include "nvmeibt_topology.h"

#define NVMEIBT_PERSISTENCY_CACHE_DIR TOMA_DIR_OPT_NVMESH "/toma/"
extern const char	toma_persistency_file_name[];

enum raft_pause_mode_enm {
    RAFT_NOT_PAUSED =  0,
    RAFT_IN_PAUSED = (0x1 << 0),
    RAFT_OUT_PAUSED = (0x1 << 1),
};

typedef XDLIST_DECLARE(, struct target_drive, target_drives_link) target_drives_spec_t;

struct nvmeibt_topology {
	int32_t								persistent_toma_software_version;	// As read from persistency
	int				 					mgmt_config_version;
	unsigned long long 					leader_config_version;
	int64_t							 	garbage_collected_applied_topo_config_idx;
	union nvmeib_uuid					mgmt_DB_uuid;
	unsigned long long					applied_topology_version;
	unsigned long long					to_apply_topology_version;
	volatile unsigned long long			committed_topology_version;
	volatile unsigned long long			submitted_topology_version;
	unsigned long long					calculated_next_topology_version;
	//
	unsigned long long					running_local_serialization_version;
	unsigned long long					known_to_leader_local_serialization_version;
	BOOL								is_leader_regeneration_required;	TODO(Replace with gap between leader_calculated and leader_to_commit);
//	BOOL								is_leader_reserialization_required;	TODO(Replace with gap between leader_calculated and leader_to_commit);
//	BOOL								is_leader_conf_reserialization_required;	TODO(Replace with gap between leader_calculated and leader_to_commit);
	BOOL								is_active_reserialization_required;	TODO(Replace with gap between leader_calculated and leader_to_commit);
	enum nvmeibt_topology_shutdown_state {
		NVMEIBT_SHUTDOWN_NONE = 0,
		NVMEIBT_SHUTDOWN_FIRST,
		NVMEIBT_SHUTDOWN
	}									shutdown_state;
	BOOL								is_any_seg_post_update_action_required;
	BOOL								is_need_calc_next_wait_for_registrant_timeout;
	int									n_stores_in_progress;
	BOOL								is_any_rebuild_progress_report_to_mgmt_due;
	BOOL								is_valid_topo_config_received;
	BOOL								is_in_shutdown_active_phase;
	unsigned long long					running_report_target_ID;
	unsigned long long					last_sent_report_target_ID;
	BOOL								should_send_segment_report;
	BOOL								is_serialize_active_topo_for_leader;	// For QA debugging
	BOOL								is_conf_corrupted;
	BOOL								prev_is_conf_corrupted;
	enum raft_pause_mode_enm			raft_pause_mode;
	struct timespec						startup_timespec;
	struct timespec						cur_event_start_time;	// Avoid some (system) calls to clock_gettime().
																//	Anyhow, nothing else happened since the start of the cur-event
																//  Not clear if better to use the event's start-time or the actual "now"
																//  since the processing-time can generate artifacts
	struct timespec						last_send_appendentries_timestamp;
	struct timespec						last_raft_distribution_timestamp;
	struct timespec						last_progress_report_timestamp;
	struct timespec						last_local_report_target_to_mgmt_time;
	struct timespec						last_global_report_to_mgmt_timespec;
	struct timespec						last_apply_time;
	struct timespec						shutdown_start_time;
	int									n_running_dirty_rebuild;
	int									n_running_stale_rebuild;
	int									n_running_txid_rebuild;
	int									n_running_cold_recovery;
	int									n_running_JGC_rebuild;
	int									n_running_scrubbing;
	// Below are pending rebuild tasks.
	int									n_pending_dirty_rebuild;
	int									n_pending_stale_rebuild;
	int									n_pending_txid_rebuild;
	int									n_pending_cold_recovery;
	int									n_pending_JGC_rebuild;
	int									n_pending_scrubbing; // Total scrubbing tasks in the heap, equal to next_scrub_timeout_heap.n_elements

	XHASHTABLE_DECLARE(, struct nvmeibt_client, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_CLIENTS_PER_NODE))		clients_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_block_device, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_BLOCK_DEVICES))	block_devices_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_nic, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_NICS))			nics_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_disk, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_DISKS))			disks_hash;
	struct nvmeib_hash_table			*disk_segments_hash_by_uuid;
	XHASHTABLE_DECLARE(, struct nvmeibt_praid, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_PRAIDS))		praids_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_node, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_NODES))			nodes_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_chunk, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_CHUNKS))		chunks_hash;
	// We need two following 'highest_seen' params because we can receive a one step backward config
	// from a new leader. In this case we prevent a wrong trim.
	int64_t						startup_timestamp_msec;
	int64_t						highest_seen_committed_kafka_mgmt_config_idx;
	int64_t						highest_seen_committed_topo_config_idx;
	int							config_tag;		// monotonic counter to mark objects added to the hash
    int							in_transmission_cnt;
    int							in_transmission_rep_cnt;
    BOOL						is_update_csv_of_config_and_topo_required;
    struct nvmeibt_Buf			buf_of_follower_wire_topo;
	struct nvmeibt_node			*my_node;
	struct HW_mgmt_conf 		*HW_mgmt_conf;
	local_disks_hash_t																									local_disks_hash;
	local_disks_hash_t																									stock_local_disks_hash;
	local_disks_hash_t																									formatting_local_disks_hash;
	XHASHTABLE_DECLARE(, struct nvmeibt_local_nic, topo_link, NVMEIB_XHASHTABLE_N_BITS(NVMEIBT_MAX_N_NICS_PER_NODE))	local_nics_hash;
	struct nvmeibt_mm					*mm;
	XDLIST_DECLARE(, struct nvmeibt_registrant_ctx, longing_link) registrants_on_invalid_seg;
	target_drives_spec_t excluded_drives_spec;
	target_drives_spec_t auto_takeover_drives_spec;
	XDLIST_DECLARE(, struct nvmeibt_udev_event_info, udev_event_info_link) udev_events_info;
	XDLIST_DECLARE(, struct nvmeibt_seg_active, global_seg_active_post_update_action_link) seg_active_post_update_action_list;
	XDLIST_DECLARE(, struct nvmeibt_praid, global_report_to_mgmt_praid_link) immediate_report_to_mgmt_praid_list;
	XDLIST_DECLARE(, struct nvmeibt_praid, praid_topo_recalc_link) praid_topo_recalc_list;
};

#define NNVMEIBT_GLOBAL_INC_N_STORES_IN_PROGRESS(__name__) do {								\
	struct nvmeibt_topology		*__cur_topo = nvmeibt_global_get_global();					\
	__cur_topo->n_stores_in_progress++;														\
	N_Tf(__name__ ## _roee, "n_stores_in_progress after INC=@N_STORES_IN_PROGRESS",			\
		 __cur_topo->n_stores_in_progress);													\
	NTOMA_ASSERT(__name__ ## _assert, __cur_topo->n_stores_in_progress < 10000, "< 10000");	\
} while (0)

#define NNVMEIBT_GLOBAL_DEC_N_STORES_IN_PROGRESS(name) do {														\
	struct nvmeibt_topology		*__cur_topo = nvmeibt_global_get_global();										\
	__cur_topo->n_stores_in_progress--;																			\
	N_Tf(name, "n_stores_in_progress after DEC = @N_STORES_IN_PROGRESS", __cur_topo->n_stores_in_progress);		\
	NTOMA_ASSERT(name ## _assert, __cur_topo->n_stores_in_progress >= 0, "< 0");								\
} while (0)

#define NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(__name__) do {										\
	struct nvmeibt_topology		*__global = nvmeibt_global_get_global();									\
	__global->running_report_target_ID++;																	\
	N_Tf(__name__, "MARK_REPORT_TARGET running_report_target_ID=@LLU", __global->running_report_target_ID);	\
} while (0)

#define NVMEIBT_GLOBAL_CLEAR_REPORT_TARGET_HAS_NEW_DATA(__name__) do {											\
	struct nvmeibt_topology		*__global = nvmeibt_global_get_global();										\
	__global->last_sent_report_target_ID = __global->running_report_target_ID;									\
	N_Tf(__name__, "CLEAR_REPORT_TARGET running_report_target_ID=@LLU", __global->running_report_target_ID);	\
} while (0)

/******* Declarations of the ".c" functions ********/

struct nvmeibt_topology *nvmeibt_global_get_global(void);
void nvmeibt_global_init(void);
void nvmeibt_global_validate_and_upd_mgmt_DB_uuid(const union nvmeib_uuid *mgmt_DB_uuid);
void nvmeibt_global_issue_leader_report_praids_status_to_mgmt(void);
void nvmeibt_global_leader_clear_old_reports_to_mgmt(void);
void nvmeibt_global_call_all_seg_active_post_update_actions(void);
void nvmeibt_global_idle_time_activities(void);
void nvmeibt_global_set_raft_pause_mode(enum raft_pause_mode_enm pause_mode);
int nvmeibt_global_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
enum nvmeibt_add_rv nvmeibt_global_parse_MGMT_CONFIG_VERSION(struct mm_mgmt_conf *conf, bool is_updating_leader);

#define NVMEIBT_GLOBAL_INC_N_TASKS_COUNTER(name, _counter_name) do {		\
	int max_counter_threshold = nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->disk_segments_hash_by_uuid);	\
	int	*p_counter = &(nvmeibt_global_get_global()->_counter_name);			\
	(*p_counter)++;															\
	N_Tf(name ## _trace, #_counter_name"=@INT", *p_counter);				\
	if (*p_counter > max_counter_threshold) {								\
		N_Wf(name ## _warning, #_counter_name" goes beyond total seg count @INT", max_counter_threshold);	\
	}																		\
} while (0)

#define NVMEIBT_GLOBAL_DEC_N_TASKS_COUNTER(name, _counter_name) do {		\
	int	*p_counter = &(nvmeibt_global_get_global()->_counter_name);			\
	--(*p_counter);															\
	N_Tf(name ## _trace, #_counter_name"=@INT", *p_counter);				\
	if (*p_counter < 0) {													\
		N_Wf(name ## _warning, "Decrementing a counter that is already 0");	\
	}																		\
} while (0)

#define NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(_counter_name) ({				\
	nvmeibt_global_get_global()->_counter_name;								\
})

/******* Static inline forward declarations ********/

static inline const union nvmeib_uuid *nvmeibt_global_get_my_node_uuid(void);
static inline struct nvmeibt_mm *nvmeibt_global_get_mm(void);
static inline struct timespec nvmeibt_global_get_cur_event_start_time(void);
static inline struct nvmeibt_seg_active *nvmeibt_global_get_seg_active_through_seg_by_uuid(const union nvmeib_uuid *uuid);

/******* Static inline with no external dependencies ********/

struct nvmeibt_global_adaptive_timeouts_ctx {
	struct nvmeib_iir		leader_calculated_topo_calc_time_ns_IIR;
	// raft
	int64_t 				follower_calculated_timeout_based_on_ping_ns;
	int64_t 				leader_calculated_append_entries_rep_time_ns;
	int64_t 				leader_calculated_topo_calc_time_ns;
	struct timespec			timespec_of_last_successful_calc_based_on_ping_response;
	// Leader topo calc
	struct {
		int						new_in_topo;
		int						with_new_dead;
		int						became_not_activated;
	} n_praids;
};
extern struct nvmeibt_global_adaptive_timeouts_ctx			my_nvmeibt_global_adaptive_timeouts;
static inline struct nvmeibt_global_adaptive_timeouts_ctx	*nvmeibt_global_adaptive_timeouts(void)
{
	return &my_nvmeibt_global_adaptive_timeouts;
}

static inline struct nvmeibt_node *nvmeibt_global_get_my_node(void)
{
	return (nvmeibt_global_get_global()->my_node);
}

static inline bool nvmeibt_topology_is_HW_config_functional(void)
{
	// We also have the raft_members config, that has a different life-cycle
	return (nvmeibt_global_get_my_node() != NULL || nvmeibt_toma_is_running_as_a_utility());
}

static inline void nvmeibt_global_set_my_node(struct nvmeibt_node *my_node)
{
	if (my_node) {
		my_node->is_my_node = 1;
	}
	nvmeibt_global_get_global()->my_node = my_node;
}

static inline void nvmeibt_global_mark_is_any_rebuild_progress_report_to_mgmt_due(void)
{
	N_Tf(57dcyw3, "");
	nvmeibt_global_get_global()->is_any_rebuild_progress_report_to_mgmt_due = 1;
}

static inline void nvmeibt_global_clear_is_any_rebuild_progress_report_to_mgmt_due(void)
{
	nvmeibt_global_get_global()->is_any_rebuild_progress_report_to_mgmt_due = 0;
}


static inline BOOL nvmeibt_global_is_any_rebuild_progress_report_to_mgmt_due(void)
{
	return nvmeibt_global_get_global()->is_any_rebuild_progress_report_to_mgmt_due;
}

static inline union nvmeib_uuid *nvmeibt_global_get_mgmt_DB_uuid(void)
{
	return &(nvmeibt_global_get_global()->mgmt_DB_uuid);
}

static inline void nvmeibt_global_set_is_serialize_active_topo_for_leader(bool is_serialize)
{
	if (nvmeibt_global_get_global()->is_serialize_active_topo_for_leader != is_serialize) {
		nvmeibt_global_get_global()->is_serialize_active_topo_for_leader = is_serialize;
		N_Tf(40bfhzo,"serialize_active_topo_for_leader=@BOOL", is_serialize);
	}
}

static inline int nvmeibt_global_get_n_stores_in_progress(void)
{
	return nvmeibt_global_get_global()->n_stores_in_progress;
}

static inline struct timespec nvmeibt_global_get_last_global_report_to_mgmt_timespec(void)
{
	return nvmeibt_global_get_global()->last_global_report_to_mgmt_timespec;
}

static inline void nvmeibt_global_set_last_global_report_to_mgmt_timespec(struct timespec val)
{
	nvmeibt_global_get_global()->last_global_report_to_mgmt_timespec = val;
}

static inline int nvmeibt_global_get_mgmt_config_version(void)
{
	return nvmeibt_global_get_global()->mgmt_config_version;
}

static inline int64_t nvmeibt_global_get_startup_timestamp_msec(void)
{
	return (nvmeibt_global_get_global()->startup_timestamp_msec);
}

static inline struct timespec nvmeibt_global_get_startup_timespec(void)
{
	return (nvmeibt_global_get_global()->startup_timespec);
}


/******* Includes needed for static inline functions ********/

#include "nvmeibt_node.h"
#include "nvmeibt_topology.h"

/******* Static inline functions that depend on other functions ******/

struct nvmeibt_mm;
// struct nvmeibt_topology;
struct nvmeibt_topology *nvmeibt_global_get_global(void);

static inline const union nvmeib_uuid *nvmeibt_global_get_my_node_uuid(void)
{
	return nvmeibt_node_UUID(nvmeibt_global_get_my_node());
}

static inline struct nvmeibt_mm *nvmeibt_global_get_mm(void)
{
	return nvmeibt_global_get_global()->mm;
}

static inline struct timespec nvmeibt_global_get_cur_event_start_time(void)
{
	return nvmeibt_global_get_global()->cur_event_start_time;
}

static inline struct nvmeibt_seg_active *nvmeibt_global_get_seg_active_through_seg_by_uuid(const union nvmeib_uuid *uuid)
{
	struct nvmeibt_disk_segment		*seg;

	seg = nvmeibt_disk_segment_get_disk_segment_by_id(uuid);
	return (seg ? nvmeibt_disk_segment_get_seg_active(seg) : NULL);
}

static inline struct nvmeibt_seg_active *nvmeibt_global_get_seg_active_through_seg_by_urn_uuid_str(char *uuid_str)
{
	union nvmeib_uuid				uuid;

	nvmeibt_urn_uuid_to_union_uuid(&uuid, (struct nvmeibt_urn_uuid *)uuid_str);
	return nvmeibt_global_get_seg_active_through_seg_by_uuid(&uuid);
}

static inline void nvmeibt_mark_conf_corrupted(void)
{
	nvmeibt_global_get_global()->is_conf_corrupted = 1;
}
static inline void nvmeibt_clear_conf_corrupted(void)
{
	nvmeibt_global_get_global()->is_conf_corrupted = 0;
}
static inline bool nvmeibt_conf_became_not_corrupted(void)
{
	bool		became_not_corrupted;

	became_not_corrupted = nvmeibt_global_get_global()->prev_is_conf_corrupted &&
						   !nvmeibt_global_get_global()->is_conf_corrupted;
	nvmeibt_global_get_global()->prev_is_conf_corrupted = nvmeibt_global_get_global()->is_conf_corrupted;
	return became_not_corrupted;
}


/******************************************************************************/

void nvmeibt_global_mark_is_reread_nvmesh_conf_required(void);
int nvmeibt_global_reread_nvmesh_conf_as_needed(void);
const char *nvmeibt_global_nvmesh_conf_get_val_by_key(const char *key);		// You cannot change the returned string. If needed, duplicate it
void nvmeibt_global_set_log_snapshotting_mode(int64_t is_log_snapshotting_mode);
int64_t nvmeibt_global_get_log_snapshotting_mode(void);
void nvmeibt_global_log_snapshotting_heuristic(void);
void nvmeibt_log_snapshotting_shutdown(void);

#endif	// #ifdef NVMEIBT_GLOBAL


