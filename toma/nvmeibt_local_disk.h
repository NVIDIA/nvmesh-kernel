#ifndef NVMEIBT_LOCAL_DISK
#define NVMEIBT_LOCAL_DISK

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_local_disk_util.h"
#include "interfaces/nvme/nvmeibt_udev.h"
#include "../common/nvmeib_shared.h"

#define NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN 		(ASCII_UUID_MAX_STR_LEN + 64)
#define MAIN_GPT_NAME								"Main"
#define METADATA_GPT_NAME							"Metadata"
#define NVMEIBT_LOCAL_DISK_MAX_MOUNTS 				128
#define NVMEIBT_LOCAL_DISK_MAX_MOUNT_NAME_LENGTH	256

struct nvmeibt_local_disk_config {
	struct nvmeibt_ascii_uuid					ldisk_id;
	struct nvmeibt_ascii_uuid					native_serial;
	unsigned int 								vendor;
	char										ld_display[100];
	uint64_t									n_pblk;
	uint64_t									n_hw_pblk;
	int 										pblk_size;
	int											max_request_size;
	int											seq;
	int											nsid;
	enum nvmeibt_disk_type						disk_type;
	char										dev_file_name[NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN];
	char										stripped_dev_file_name[NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN];
	int 										metadata_n_bytes;
	char										status[NVMEIBS_DISKS_CSV_STATUS_LEN];
	struct nvmeibt_local_disk_util_smart_info	smart_info;
	struct nvmeibt_local_disk_util_smart_info	prev_smart_info;
	struct nvmeibt_disk_metadata				disk_metadata;
	char										pcie_slot[NVMEIBS_DISKS_CSV_STATUS_LEN];
	char										pcie_bdf[NVMEIBS_DISKS_CSV_STATUS_LEN];
};

struct format_details {
	struct nvmeibt_ascii_uuid							ldisk_id;
	struct nvmeibt_ascii_uuid							native_serial;
	char												ld_display[100];
	int													nsid;
	unsigned int 										vendor_id;
	unsigned int 										block_size;
	uint64_t	 										n_pblk;
	unsigned int 										is_nvme;
	unsigned int 										metadata_size;
	union nvmeib_uuid									disk_obj_uuid;
	unsigned int										format_request_counter;
	char 												model[ASCII_UUID_MAX_STR_LEN];
};

struct nvmeibt_local_disk_CHANGE_EVENT_counters {
	unsigned int 		last_CHANGE_no;
	unsigned int 		active_zeroing_CHANGE_no;
};

struct nvmeibt_udev_event_info {
	struct xdlist						udev_event_info_link;
	char								dev_file_name[NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN];
	enum nvmeibt_disk_type				disk_type;
	enum nvmeibt_udev_event_action		act_to_process;
	enum nvmeibt_udev_event_action		act_pending;
	BOOL								is_processing;
};

struct nvmeibt_local_disk {
	struct nvmeibt_local_disk_config					from_config;
	struct nvmeibt_disk									*its_disk;
	struct nvmeibt_udev_event_info						*its_udev_event_info; // exists only during bind/unbind triggered by udev_event
	struct mmap_tbl										mmap_disk_locks_tbl;
	struct nvmeibt_disk_mbr								mbr;
	struct nvmeibt_disk_gpt 							main_gpt;
	struct nvmeibt_disk_gpt 							metadata_gpt;
	int													config_tag;
	struct xdlist										controller_local_disks_list_link;
	struct nvmeibt_local_disk_controller				*controller;
	BOOL												is_being_deleted;	// Used as optimization to avoid noise in the log, turned early in the removal process to avoid new tasks on the local disk.
	BOOL												should_be_removed;  // Marks that the disk is at the end of it's current life and should be freed, once all operations/recoveries complete.
	BOOL												is_conf_corrupted;
	BOOL												is_drive_write_error;
	int													gpt_change_no;
	int													gpt_submitted_change_no;	// If behind then needs writing
	BOOL												is_mbr_a_valid_pmbr;
	BOOL												is_bind_to_nvmeibs_needed;
	BOOL												is_binding_to_nvmeibs;
	BOOL												is_bind_back_to_stock_needed;
	BOOL												is_binding_back_to_stock;
	int													dev_file_fd;
	struct netlink_io_context							*dev_nl_ctx;
	BOOL												is_excluded;
	BOOL												is_auto_takeover;
	BOOL												is_explicitly_excluded;
	BOOL												should_relaunch_format_on_the_next_zeroing_finalize;
	BOOL												are_partitions_setup_in_mem;
	BOOL												is_PMBR_saved_on_disk;
	BOOL												is_owned_by_nvmeibs_driver;
	struct format_details								pending_format;
	enum nvmeibs_serjio_status							serjio_status;
	BOOL												is_being_formatted;
	BOOL												is_smart_log_valid;
	BOOL												is_done_reading_gpt_existing_or_not;
    struct timespec										last_periodic_reread_smart_counters_time;
	struct timespec										last_add_time;
	BOOL												is_periodic_reread_smart_counters_in_the_air;
	BOOL												was_last_read_of_smart_counters_successful;
	BOOL												is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs;
	BOOL												prev_is_ready_for_segments;
    struct timespec										last_zeroing_update_time;
	unsigned int										active_format_request_counter;
	unsigned int										reappearing_counter;
	struct nvmeibt_local_disk_CHANGE_EVENT_counters		CHANGE_EVENT_counters;
	struct nvmeib_hash_table							*seg_active_hash_by_uuid;
};

#define LOCAL_DISK_LOG_FMT "disk=@STR(@STR.@INT)"
#define LOCAL_DISK_LOG_ARGS(ldisk) nvmeibt_local_disk_UUID_str(ldisk), (ldisk)->from_config.native_serial.str, (ldisk)->from_config.nsid
#define LOCAL_DISK_LOG_obj_ARGS(obj) (obj)->ldisk_id.str, (obj)->native_serial.str, (obj)->nsid

typedef XDLIST_DECLARE(local_disks_list, struct nvmeibt_local_disk, controller_local_disks_list_link) nvmeibt_local_disk_controllers_local_disks_list_t;

struct nvme_id_ctrl;
struct nvme_id_ns;
struct nvmeibt_topology;

#define NNVMEIBT_LOCAL_DISK_GET_DISK(name, __local_disk__) ({								\
	struct nvmeibt_disk		*__disk = (__local_disk__ ? __local_disk__->its_disk : NULL);	\
	if (!__disk && nvmeibt_topology_is_HW_config_functional()) {							\
		N_Tf(name, "No disk for disk=@STR in config yet",				 					\
		nvmeibt_local_disk_display(__local_disk__));										\
	}																						\
	__disk;																					\
})

TODO("notlttng __new_version__ -> __version__ + 1");
#define NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(name, __local_disk__)	do {								\
	if (__local_disk__) {																				\
		int		__gpt_no__ = (__local_disk__)->gpt_change_no;											\
		int 	__new_gpt_no__ = __gpt_no__ + 1;	 													\
		N_Tf(name, "INC_GPT_CHANGE_NO disk=@STR (@INT-->@INT)",											\
			nvmeibt_local_disk_display(__local_disk__), __gpt_no__, __new_gpt_no__);					\
		(__local_disk__)->gpt_change_no = __new_gpt_no__;												\
		NTOMA_ASSERT(name ## _assert, 																	\
					 (__local_disk__)->gpt_change_no - (__local_disk__)->gpt_submitted_change_no < 10000,\
					"disk=@LOCAL_DISK gpt_change_no=@INT "												\
					"gpt_submitted_change_no=@INT",														\
					nvmeibt_local_disk_display(__local_disk__),											\
					(__local_disk__)->gpt_change_no, (__local_disk__)->gpt_submitted_change_no);		\
	} else {																							\
		N_Ef(name ## _error, "INC_GPT_CHANGE_NO local_disk=NULL)");										\
	}																									\
} while (0)

static inline int nvmeibt_local_disk_get_GPT_CHANGE_NO(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? local_disk->gpt_change_no : -1);
}

#define NNVMEIBT_LOCAL_DISK_SET_GPT_SUBMITTED_CHANGE_NO(name, __local_disk__, __new_value__)	do {			\
	if (__local_disk__) {																						\
		N_Tf(name##_T, "SET_GPT_SUBMITTED_CHANGE_NO disk=@STR (@INT-->@INT)",									\
			nvmeibt_local_disk_display(__local_disk__),															\
			(__local_disk__)->gpt_submitted_change_no, (__new_value__));										\
		(__local_disk__)->gpt_submitted_change_no = (__new_value__);											\
	} else {																									\
		N_Ef(name##_E, "SET_GPT_SUBMITTED_CHANGE_NO local_disk=NULL)");											\
	}																											\
} while (0)

enum NVMEIBT_LOCAL_DISK_CONTROLLER_STATE {
	NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNDEFINED = 0,
	NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK = 1,
	NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_STOCK_TO_NVMEIBS = 2,
	NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS = 3,
	NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_NVMEIBS_TO_STOCK = 4,
};

struct nvmeibt_local_disk_controller {
	char													native_serial_str[NVMEIBS_DISKS_CSV_STATUS_LEN];
	enum NVMEIBT_LOCAL_DISK_CONTROLLER_STATE				controller_state;
	struct xdlist											all_controllers_list_link;
	nvmeibt_local_disk_controllers_local_disks_list_t		local_disks_list;
	bool													is_bind_to_nvmeibs_needed;
	bool													is_bind_back_to_stock_needed;
	bool													is_excluded;
	bool													is_auto_takeover;
};

typedef XDLIST_DECLARE(controllers_list, struct nvmeibt_local_disk_controller, all_controllers_list_link) nvmeibt_local_disk_controllers_list_t;

static inline int nvmeibt_local_disk_get_GPT_SUBMITTED_CHANGE_NO(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? local_disk->gpt_submitted_change_no : -1);
}

void nvmeibt_local_disk_fill_ld_info(struct local_disk_info	*ld_info, struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_set_disable_periodic_smart_polling(int64_t val);
int nvmeibt_local_disk_one_time_init(void);
const char *nvmeibt_local_disk_config_display(const struct nvmeibt_local_disk_config *config);
const char *nvmeibt_local_disk_display(const struct nvmeibt_local_disk *local_disk);
u16 nvmeibt_local_disk_vendor_id(const struct nvmeibt_local_disk *local_disk);
int nvmeibt_local_disk_dev_file_fd(const struct nvmeibt_local_disk *local_disk);
struct netlink_io_context *nvmeibt_local_disk_dev_nl_ctx(const struct nvmeibt_local_disk *local_disk);
const struct nvmeibt_ascii_uuid *nvmeibt_local_disk_UUID(const struct nvmeibt_local_disk *local_disk);
static inline const char *nvmeibt_local_disk_UUID_str(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? nvmeibt_local_disk_UUID(local_disk)->str : NULL);
}
const char *nvmeibt_local_disk_file_name(const struct nvmeibt_local_disk *local_disk);
BOOL nvmeibt_local_disk_is_md_supported(const struct nvmeibt_local_disk *local_disk);
BOOL nvmeibt_local_disk_is_ready_for_segments(const struct nvmeibt_local_disk *local_disk);
static inline bool nvmeibt_local_disk_is_connected_to_disk(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk && local_disk->its_disk);
}
BOOL nvmeibt_local_disk_is_done_initial_reading_of_local_disk(const struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_periodic_reread_smart_counters_just_finished(struct nvmeibt_local_disk *local_disk, BOOL is_successful, BOOL is_periodic_reread_needed);
int nvmeibt_local_disk_recover_missing_ldisk_id_in_upgraded_disk_metadata(struct nvmeibt_local_disk_config *f);
enum nvmeibt_add_rv nvmeibt_local_disk_add_from_config(char *config_str, int config_tag);
int nvmeibt_local_disk_add_from_stock_driver(struct nvmeibt_udev_event_info *udev_event_info);
BOOL nvmeibt_local_disk_is_being_deleted(const struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_is_being_deleted(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(struct nvmeibt_local_disk *local_disk);
int nvmeibt_local_disk_free_all_resources(void);
void nvmeibt_local_disk_free_stock_fds(void);
void nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_clear_is_bind_to_nvmeibs_needed(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_remove_stock_local_disk_by_dev_file_name(const char *dev_file_name);
void nvmeibt_local_disk_remove_by_ldisk_id_str(const char *ldisk_id);
struct nvmeibt_local_disk *nvmeibt_stock_local_disk_get_by_dev_file_name(const char *dev_file_name, bool is_mandatory);
struct nvmeibt_local_disk *nvmeibt_local_disk_get_by_dev_file_name(const char *dev_file_name);
BOOL nvmeibt_local_disk_is_bind_to_nvmeibs_needed(const struct nvmeibt_local_disk *local_disk);

BOOL nvmeibt_local_disk_is_serjio_ready(const struct nvmeibt_local_disk *local_disk);
int nvmeibt_local_disk_update_serjio_state(const char *ldisk_id_str, const char *native_serial_str, int nsid, enum nvmeibs_serjio_status serjio_status);
struct nvmeibt_local_disk *nvmeibt_local_disk_get_local_disk_by_ldisk_id(const struct nvmeibt_ascii_uuid *ldisk_id, struct nvmeib_hash_table *local_disks_hash_by_ldisk_id_str);
void nvmeibt_local_disk_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
BOOL nvmeibt_local_disk_is_formatted(const struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_launch_local_disk_periodic_reread_smart_counters_if_needed(struct nvmeibt_local_disk *local_disk, bool is_stock_disk);
int nvmeibt_local_disk_record_zeroing_progress(struct nvmeibt_local_disk *local_disk, uint64_t last_pba_zeroed);

void nvmeibt_local_disk_launch_bind_to_nvmeibs(struct nvmeibt_local_disk *stock_local_disk);
void nvmeibt_local_disk_launch_bind_back_to_stock(struct nvmeibt_local_disk *local_disk);

int nvmeibt_local_disk_launch_disk_format(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_should_be_removed(struct nvmeibt_local_disk *local_disk);
BOOL nvmeibt_local_disk_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_clear_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk);


void nvmeibt_local_disk_mark_is_bind_back_to_stock_needed(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_clear_is_bind_back_to_stock_needed(struct nvmeibt_local_disk *local_disk);
BOOL nvmeibt_local_disk_is_bind_back_to_stock_needed(const struct nvmeibt_local_disk *local_disk);
BOOL nvmeibt_local_disk_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_clear_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk);
int nvmeibt_local_disk_write_PMBR_and_GPTs(struct local_disk_info *ld_info);
void nvmeibt_local_disk_mark_segs_post_update_actions_required(struct nvmeibt_local_disk *local_disk);
void nvmeibt_local_disk_mark_is_specific_disk_report_req(const char *ldisk_id, unsigned int reappearing_counter);
void nvmeibt_local_disk_stop_all_activities(struct nvmeibt_disk *disk);


/******************* API Toma-Srvr for zeroing/formating **********************/
struct nvmeibt_disk_flow_params_t {
	char model[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];	// Disk model to which those params are applicable
	/*BOOL*/ u32 is_zeroing_using_test_and_write;  	// default=true
	/*BOOL*/ u32 is_using_nvme_trim_before_zero;  	// default=true
	/*BOOL*/ u32 is_secure_erase_after_disk_format;	// default=true
	/*BOOL*/ u32 is_zeroing_mandatory;				// default=true
	/*BOOL*/ u32 delete_ns_when_formatting;			// default=false
	/*BOOL*/ u32 reset_after_format;				// default=false
	/*BOOL*/ u32 skip_reformat;						// default=false
	/*BOOL*/ u32 ignore_metadata;					// default=false
	/*BOOL*/ u32 force_metadata;					// default=false
	/*BOOL*/ u32 force_512b;						// default=false
};

struct nvmeibt_disks_models_flow_params_t {
	int n_models;									// Specific params per model
	struct nvmeibt_disk_flow_params_t dm[10];		// Array of disk models
	struct nvmeibt_disk_flow_params_t dflt;			// Default value, to unspecified models
};

int nvmeibt_disk_flow_params_try_read_from_config_line(const char*config, int *n_matches);		// Load parameters from config line
const struct nvmeibt_disk_flow_params_t *nvmeibt_disk_flow_params_get(const char *model, bool use_defaults);
const struct nvmeibt_disk_flow_params_t *nvmeibt_disk_flow_params_get_next_model(const struct nvmeibt_disk_flow_params_t *arg);
int nvmeibt_disk_flow_params_set_model_params(struct nvmeibt_disk_flow_params_t *arg);
void nvmeibt_disk_flow_params_remove_model(const char *model);
void nvmeibt_disk_flow_params_print(struct nvmeibt_Str *s);
void nvmeibt_disk_flow_params_reset_models_before_new_scan(void);

struct nvmeibt_ldisk_id_for_srvr_cmd nvmeibt_local_disk_config_to_srvr_cmd_disk(const struct nvmeibt_local_disk_config *ld);
void nvmeibt_local_disk_set_read_cnt_test(bool read_cnt_test);

#define dev_dir					TOMA_ROOT_DIR "dev/"
#define nvme_dev_name			"nvme"
#define sata_dev_name			"sd"
#define nvme_dev_name_prefix	dev_dir nvme_dev_name
#define sata_dev_name_prefix	dev_dir sata_dev_name

#define IS_SUPPORTED_NOT_NVME_DISK_TYPE(d_t) \
	((d_t == NVMEIBT_EXTERNAL_DISK_TYPE) || (d_t == NVMEIBT_VIRTUAL_DISK_TYPE))
#define IS_SUPPORTED_NVME_DISK_TYPE(d_t) \
	((d_t == NVMEIBT_NVME_DISK_TYPE) || (d_t == NVMEIBT_NVMESH_DISK_TYPE))
#define IS_SUPPORTED_STOCK_DISK_TYPE(d_t) \
	((d_t == NVMEIBT_EXTERNAL_DISK_TYPE) || (d_t == NVMEIBT_VIRTUAL_DISK_TYPE) || (d_t == NVMEIBT_NVME_DISK_TYPE))

static inline bool is_supported_not_nvme_disk(const struct nvmeibt_local_disk *local_disk)
{
	enum nvmeibt_disk_type					disk_type;

	disk_type = local_disk->from_config.disk_type;
	return IS_SUPPORTED_NOT_NVME_DISK_TYPE(disk_type);
}

static inline bool is_supported_nvme_disk(const struct nvmeibt_local_disk *local_disk)
{
	enum nvmeibt_disk_type					disk_type;

	disk_type = local_disk->from_config.disk_type;
	return IS_SUPPORTED_NVME_DISK_TYPE(disk_type);
}

static inline int nvmeibt_local_disk_pblk_size(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? local_disk->from_config.pblk_size : 0);
}

static inline void nvmeibt_local_disk_mark_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs = 1;
}

static inline bool nvmeibt_local_disk_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? local_disk->is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs : 0);
}

static inline uint64_t nvmeibt_local_disk_get_last_byte_zeroed(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? ((local_disk->from_config.disk_metadata.last_pba_zeroed + 1) * nvmeibt_local_disk_pblk_size(local_disk)) - 1 : 0);
}


static inline unsigned int nvmeibt_local_disk_get_active_format_request_counter_to_report(struct nvmeibt_local_disk *local_disk)
{
	unsigned int				counter_to_report;

	if (local_disk->from_config.disk_metadata.format_request_counter > local_disk->active_format_request_counter) {
		counter_to_report = local_disk->from_config.disk_metadata.format_request_counter;
	} else {
		counter_to_report = local_disk->active_format_request_counter;
	}
	return counter_to_report;
}

#endif // #ifndef NVMEIBT_LOCAL_DISK

