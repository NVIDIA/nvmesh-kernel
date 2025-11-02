#ifndef NVMEIBT_BLOCK_DEVICE
#define NVMEIBT_BLOCK_DEVICE

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_mm_json.h"

struct nvmeibt_chunk;
#define NVMEIBT_BLOCK_DEVICE_UNINITIALIZED_ENCRYPT_IDX				(-1)

struct nvmeibt_block_device_atributes {
	int							version;					// ++ by mgmt when attribute changes
	int							relative_rebuild_priority;	// Values 1..10
};

struct nvmeibt_block_device_config {
	union nvmeib_uuid			id;
	int							version;					// ++ by mgmt when layout changes
	char 						client_blkdev_name[32];
	int							n_chunks;
	long long					size_lblks;
	int							blk_size_bytes;
	BOOL						is_deprecated;
	int64_t						mgmt_config_kafka_offset_or_idx;
	struct nvmeibt_block_device_atributes attr;
	bool						enableCrcCheck;
	bool						use_debug_di;
	uint8_t 					stripe_size;
	uint8_t 					stripe_width;
};

struct run_exec_on_blkdev_ctx;
typedef void (*run_exec_on_blkdev_cb_t)(struct run_exec_on_blkdev_ctx *ctx);

struct run_exec_on_blkdev_ctx {
	// Inputs
	struct nvmeibt_block_device		*blkdev;
	char							executable_str[MAX_EXEC_WITH_ARGS_STR_LEN];
	int								timeout_ms;
	run_exec_on_blkdev_cb_t			run_exec_on_blkdev_cb_func;
	// Outputs
	struct nvmeibt_Str				*child_stdout_buf;
	struct nvmeibt_Str				*child_stderr_buf;
	int 							toma_rv;
	int 							exec_rv;
};

#define PASSPHRASE_MAX_LEN 4197
#define PASSPHRASE_DIR_NAME TOMA_ROOT_DIR "root/nvmesh_toma_tmp"
struct nvmeibt_encrypt_params {
	struct run_exec_on_blkdev_ctx			exec_ctx;
	struct nvmeibt_block_device				*origin_vol;
	int64_t									kafka_offset;
	int										encrypt_idx;
	char									old_passphrase[PASSPHRASE_MAX_LEN];
	char									old_passphrase_file_name[PATH_MAX];
	char									new_passphrase[PASSPHRASE_MAX_LEN];
	char									new_passphrase_file_name[PATH_MAX];
};

struct nvmeibt_block_device {
	struct nvmeibt_block_device_config		from_config;
	struct nvmeibt_urn_uuid					urn_uuid;
	struct mm_vol_conf						serialized_vol_conf;	// Only one such. No lots for topo_config and kafka_mgmt_config.
	struct nvmeibt_chunk					*chunks[NVMEIBT_MAX_N_CHUNK_PER_BLOCK_DEVICE];
	struct nvmeibt_Buf						kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf;
	int										n_chunks;
	BOOL									is_being_deleted;
	uint8_t									trim_flags;
	int										config_tag;
	int										encrypt_idx;
	struct nvmeibt_encrypt_params 			*encrypt_params;
	struct xdlist							topo_link;
};

static inline const char *nvmeibt_blkdev_name(const struct nvmeibt_block_device *b) {
	return (b ? b->from_config.client_blkdev_name : "???");
}

static inline bool nvmeibt_blkdev_is_being_deleted(const struct nvmeibt_block_device *b) {
	return (!b || b->is_being_deleted);
}

bool nvmeibt_block_device_is_deprecated_in_config( const struct nvmeibt_block_device *block_device);
const union nvmeib_uuid *nvmeibt_block_device_UUID(const struct nvmeibt_block_device *block_device);
const char *nvmeibt_block_device_id_str(const struct nvmeibt_block_device *block_device);
enum nvmeibt_add_rv nvmeibt_block_device_add(struct mm_vol_conf *vol, int config_tag, struct nvmeibt_block_device **output_block_device, bool is_topo_config);
struct nvmeibt_block_device *nvmeibt_block_device_get_block_device_by_id(const union nvmeib_uuid *block_device_id);
void nvmeibt_block_devices_garbage_collect(bool *is_any_garbage_collected, bool *is_all_garbage_collected);
void nvmeibt_block_device_trim_specific_block_device(struct nvmeibt_block_device *block_device, uint8_t trim_flag);
void nvmeibt_block_device_trim_unused_entries(int config_tag, uint8_t trim_flag);
int nvmeibt_block_device_validate_blkdevs_config(void);
void nvmeibt_block_device_reservation_mode_change(const union nvmeib_uuid *vol_uuid, uint64_t reservation_version);
int nvmeibt_block_device_print_blkdevs_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
void nvmeibt_block_device_print_zeroing_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
#endif // #ifndef NVMEIBT_BLOCK_DEVICE

