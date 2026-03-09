#ifndef NVMEIBT_TOMA
#define NVMEIBT_TOMA

#include "nvmeibt_common.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_debug.h"
#include "clnt/nvmeibt_client_protocol.h"

#define TOMA_CONFIG_REREAD_TIMEOUT_SEC 5

extern bool nvmeibt_use_libibcm;
extern bool nvmeibt_ib_use_srq;

extern char	nvmeibt_toma_cmdline_arg_input_file_name[PATH_MAX];
extern char	nvmeibt_toma_cmdline_arg_output_file_name[PATH_MAX];
extern bool	nvmeibt_is_converting_json_to_persistence;
extern bool	nvmeibt_is_converting_persistence_to_json;


enum NVMEIBT_TOMA_WAKEUP_TYPE {
	NVMEIBT_TOMA_WAKEUP_TYPE_WQ			= 2,
	NVMEIBT_TOMA_FD_TYPE_LOCAL_SERVER_EVENTS = 3,
	NVMEIBT_TOMA_WAKEUP_TYPE_NETLINK	= 4,
	NVMEIBT_TOMA_WAKEUP_TYPE_KAFKA		= 5,
	NVMEIBT_TOMA_WAKEUP_TYPE_LAST		= 6,
};

struct nvmeibt_topology;
struct nvmeibt_big_msg;
struct nvmeibt_registrant_ctx;
struct nvmeibt_node;
struct km_comm_msg_hdr;
struct nvmeibt_udev_event_info;
struct nvmeibt_raft_member;

bool nvmeibt_toma_is_running_as_a_utility(void);
void nvmeibt_toma_mark_is_need_to_update_the_main_select_fds(void);
struct nvmeibt_topology *nvmeibt_global_get_global(void);
void nvmeibt_toma_dispatch_received_msg(struct nvmeibt_big_msg *big_msg);
void nvmeibt_toma_raft_validity_was_updated(void);
void nvmeibt_toma_leader_mark_member_non_responsive(struct nvmeibt_raft_member *target);
void nvmeibt_toma_init_mesh(void);
int nvmeibt_toma_send_msg_to_client(struct nvmeibt_registrant_ctx *reg_ctx, int praid_version,
									enum NVMEIBT_CLIENT_MSG_TYPES msg_type, enum NVMEIBT_CLIENT_TR_REASON reason, int data_length, void *data, u64 msg_id);
void nvmeibt_toma_set_main_thread(void);
bool nvmeibt_toma_is_main_thread(void);
const char *nvmeibt_toma_get_executable_dir(void);
const char *nvmeibt_toma_get_log_dir_name(void);
int nvmeibt_toma_get_n_log_file(void);
long long nvmeibt_toma_get_log_file_max_size(void);
unsigned int nvmeibt_toma_get_bin_log_file_n(void);
unsigned int nvmeibt_toma_get_bin_log_file_size(void);
void *nvmeibt_toma_extract_user(void *buf);
void nvmeibt_toma_on_new_disk(void);

bool nvmeibt_toma_is_in_shutdown(void);
bool is_shutdown_me_only(void);

int nvmeibt_toma_is_single_instance(void);
void nvmeibt_toma_cleanup_single_instance(void);

struct nvmeibt_wq_entry;
void nvmeibt_toma_wakeup_wq_abort_func(struct nvmeibt_wq_entry *wq_entry);
int  nvmeibt_toma_trigger_wakeup(           enum NVMEIBT_TOMA_WAKEUP_TYPE type, void *ptr);		// Todo: Remove this eventually
void nvmeibt_toma_trigger_wakeup_handle_err(struct nvmeibt_wq_entry *wq_entry);
int nvmeibt_toma_persistency_add_work(struct nvmeibt_wq_entry *e);
int nvmeibt_toma_leader_add_work(struct nvmeibt_wq_entry *e);
int nvmeibt_registrant_disconnect_add_work(struct nvmeibt_local_disk *local_disk, struct nvmeibt_wq_entry *e);
int nvmeibt_toma_read_disk_from_stock_driver_add_work(struct nvmeibt_wq_entry *e);
void nvmeibt_toma_abort_child_processes(void);
void nvmeibt_topology_set_mgmt_updates_pause_state(int is_paused);

void nvmeibt_server_lib_create(void);
void nvmeibt_server_lib_consume_incomming_srvr_msgs(void);
int nvmeibt_send_msg_to_srv(struct km_comm_msg_hdr *msg);
void nvmeibt_server_lib_destroy(void);

struct format_details;
int nvmeibt_toma_get_status_str(enum nvmeibs_toma_status_type status_type, struct nvmeibt_Str *out);
int nvmeibt_toma_get_real_time_errors_str(struct nvmeibt_Str *out);
void nvmeibt_toma_udev_event_processing_end(struct nvmeibt_udev_event_info *udev_event_info);
void nvmeibt_toma_process_waiting_udev_events(void);

struct nvmeibt_nm_local_node;
struct nvmeibt_nm_local_node * nvmeibt_get_nw_node(void);
void wakeup_format_event(const struct nvmeibt_ascii_uuid *ldisk_id,
						 unsigned int vendor_id,
						 const char *format_req_disk_obj_uuid_str,
						 unsigned int block_size,
						 unsigned int metadata_size,
						 unsigned int format_request_counter,
						 int64_t boot_time,
						 const struct nvmeibt_urn_uuid *mgmt_DB_urn_uuid,
						 const struct nvmeibt_ascii_uuid *native_serial,
						 int nsid,
						 const char *native_nguid __attribute__((unused)));
#endif

