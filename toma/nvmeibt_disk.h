#ifndef NVMEIBT_DISK
#define NVMEIBT_DISK

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_seg_active.h"

struct nvmeibt_disk_config {
	union nvmeib_uuid			id;
	unsigned int				vendor_id;
	int							disk_version;
	struct nvmeibt_ascii_uuid	ldisk_id;
	union nvmeib_uuid			its_original_node_id;
	uint64_t					n_pblks;
	int							is_out_of_service;
	unsigned int				active_format_request_counter;
};

struct nvmeibt_node;
struct nvmeibt_disk_segment;
struct nvmeibt_local_disk;

struct nvmeibt_disk {
	struct nvmeibt_disk_config		from_config;
	struct nvmeibt_urn_uuid			urn_uuid;
	struct nvmeibt_raft_member		*leader_its_raft_member;
	struct nvmeibt_node				*its_node_config;
	struct nvmeibt_disk_segment		**disk_segments;
	int								n_segments;
	int								n_allocated_segments;
	struct nvmeibt_local_disk		*its_local_disk;
	struct xdlist					topo_link;
	int								config_tag;
	int								serialization_signature;
	BOOL							is_drive_write_error;
	BOOL							is_needed_for_vol;
};

/********* Declarations of the ".c" functions *********/

struct nvmeibt_node *nvmeibt_disk_get_node(const struct nvmeibt_disk *disk);
int nvmeibt_disk_get_version(const struct nvmeibt_disk *disk);

struct nvmeibt_local_disk *nvmeibt_disk_get_local_disk(const struct nvmeibt_disk *disk);

struct nvmeibt_disk* nvmeibt_disk_get_disk_by_ldisk_id(const struct nvmeibt_ascii_uuid *ldisk_id);
const char *nvmeibt_disk_get_ldisk_id_str(const struct nvmeibt_disk *disk);
const struct nvmeibt_ascii_uuid *nvmeibt_disk_get_ldisk_id(const struct nvmeibt_disk *disk);
const char *nvmeibt_disk_get_ldisk_display(const struct nvmeibt_disk *disk);
unsigned int nvmeibt_disk_vendor_id(const struct nvmeibt_disk *disk);
const union nvmeib_uuid *nvmeibt_disk_UUID(struct nvmeibt_disk *disk);
const char *nvmeibt_disk_id_str(struct nvmeibt_disk *disk);

char *nvmeibt_disk_get_leader_node_name(struct nvmeibt_disk *disk);
char *nvmeibt_disk_get_applied_node_name(struct nvmeibt_disk *disk);
char *nvmeibt_disk_get_node_name(const struct nvmeibt_disk *disk);
enum nvmeibt_add_rv nvmeibt_disk_add(struct mm_disk_conf *conf, int idx);

bool nvmeibt_disk_is_local_in_config(const struct nvmeibt_disk *disk);
bool nvmeibt_disk_is_local(const struct nvmeibt_disk *disk);

void nvmeibt_disk_leader_detach_all_disks_from_raft_members(void);
void nvmeibt_disk_detach_from_node(struct nvmeibt_disk *disk);
int nvmeibt_disk_brute_force_del_all_segs_due_to_format(struct nvmeibt_disk *disk);
struct nvmeibt_seg_active *nvmeibt_disk_get_seg_active(struct nvmeibt_disk *disk, const union nvmeib_uuid *seg_uuid);
void nvmeibt_disk_trim_unused_entries(int n_used);
bool nvmeibt_disk_serialize_config_line(struct mm_disk_conf *disk_conf, struct nvmeibt_disk *disk, int serialization_signature);

int nvmeibt_disk_print_status_line(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_disk *disk, int n_prefix_tabs);
int nvmeibt_disk_print_disks_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

/******* Static inline forward declarations ********/

/******* Static inline with no external dependencies ********/

static inline bool nvmeibt_disk_is_explicitly_out_of_service(const struct nvmeibt_disk *disk)
{
	return (disk && disk->from_config.is_out_of_service);
}

/******* Includes needed for static inline functions ********/

#include "nvmeibt_node.h"

/******* Static inline functions that depend on other functions ******/

#endif	// #ifdef NVMEIBT_DISK

