#include "nvmeibt_disk.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"

struct nvmeibt_local_disk *nvmeibt_disk_get_local_disk(const struct nvmeibt_disk *disk)
{
	return (disk ? disk->its_local_disk : NULL);
}

const char *nvmeibt_disk_get_ldisk_id_str(const struct nvmeibt_disk *disk);
const char *nvmeibt_disk_get_ldisk_display(const struct nvmeibt_disk *disk)
{
	return (disk ? nvmeibt_local_disk_display(nvmeibt_disk_get_local_disk(disk)) : "???");
}

const struct nvmeibt_ascii_uuid* nvmeibt_disk_get_ldisk_id(const struct nvmeibt_disk *disk)
{
	return (disk ? &(disk->from_config.ldisk_id) : NULL);
}

const char *nvmeibt_disk_get_ldisk_id_str(const struct nvmeibt_disk *disk)
{
	return (disk ? disk->from_config.ldisk_id.str : "???");
}

unsigned int nvmeibt_disk_vendor_id(const struct nvmeibt_disk *disk)
{
	return (disk ? disk->from_config.vendor_id : 0);
}

const union nvmeib_uuid *nvmeibt_disk_UUID(struct nvmeibt_disk *disk)
{
	return (disk ? &disk->from_config.id : &nvmeib_uuid_null_val);
}

const char *nvmeibt_disk_id_str(struct nvmeibt_disk *disk)
{
	return (disk ? disk->urn_uuid.str : "");
}

char *nvmeibt_disk_get_leader_node_name(struct nvmeibt_disk *disk)
{
	return (disk ? nvmeibt_raft_member_name(disk->leader_its_raft_member) : "");
}

char *nvmeibt_disk_get_applied_node_name(struct nvmeibt_disk *disk)
{
	return (disk ? nvmeibt_node_name(disk->its_node_config) : "");
}

char *nvmeibt_disk_get_node_name(const struct nvmeibt_disk *disk)
{
	return (disk ? nvmeibt_node_name(disk->its_node_config) : "");
}

struct nvmeibt_disk* nvmeibt_disk_get_disk_by_ldisk_id(const struct nvmeibt_ascii_uuid *ldisk_id)
{
	struct nvmeibt_disk		*disk = NULL;

	if (ldisk_id == NULL) {
		goto out;
	}
	NVMEIB_HASH_FOREACH(disk, nvmeibt_global_get_global()->disks_hash_by_uuid) {
		if (is_ascii_uuid_eq(ldisk_id, nvmeibt_disk_get_ldisk_id(disk))) {
			goto out;
		}
	}
	N_Tf(hsiek3k, "disk not found in config @STR", ldisk_id->str);
	disk = NULL;
out:
	return disk;
}

#ifdef TOMA_DEBUG
void nvmeibt_disk_dump(struct nvmeibt_disk *disk)
{
	struct nvmeibt_disk_config *f = &disk->from_config;

	N_Tf(trace_disk_nvmeibt_disk_dump, "Config data: id=@UUID_LE version=@INT name=@NAME its_orig_node_id=@UUID_LE",
		 &f->id, f->disk_version, f->ldisk_id.str, &f->its_original_node_id);
}
#else	// #ifdef TOMA_DEBUG
void nvmeibt_disk_dump(__attribute__((__unused__)) struct nvmeibt_disk *disk) {}
#endif	// #ifdef TOMA_DEBUG

static void disk_remove(struct nvmeibt_disk *disk)
{
	NFIN;
	if (disk == NULL) {
		goto out;
	}

	N_Tf(trace_disk_disk_remove, "Removing disk=@UUID_LE", nvmeibt_disk_UUID(disk));

	NNVMEIBT_HASH_DEL_OBJ_new(trace_1_disk_disk_remove, nvmeibt_global_get_global()->disks_hash_by_uuid, disk, disk);
	NNVMEIBT_BM_FREE(trace_2_disk_disk_remove, disk);

out:
	NFOUT;
}

enum nvmeibt_add_rv nvmeibt_disk_add(struct mm_disk_conf *conf, int config_tag)
{
	enum nvmeibt_add_rv			rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_disk			*new_disk, *disk = NULL;	// Read into it, maybe use it.
	struct nvmeibt_disk_config	*f = NULL;

	NFIN;

	new_disk = NNVMEIBT_BM_CALLOC(trace_disk_nvmeibt_disk_add, sizeof *new_disk);

	f = &(new_disk->from_config);

	f->id = conf->uuid;
	f->disk_version = conf->version;
	strncpy(f->ldisk_id.str, conf->diskID, sizeof(f->ldisk_id.str));
	f->vendor_id = conf->vendorID;
	f->its_original_node_id = conf->origNodeUuid;
	f->n_pblks = conf->n_pblks;
	f->is_out_of_service = conf->isOutOfService;
	f->active_format_request_counter = conf->activeFormatRequestCounter;

	if ((int)f->vendor_id == -1) {
		N_Wf(mmq339c, "got disk with id=@UUID_LE and vendor_id=-1 from MGMT probably from upgrade, skipping for now", &f->id);
		rv = NVMEIBT_ADD_SKIPPED;
		goto out;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ_new(trace_1_disk_nvmeibt_disk_add,
					nvmeibt_global_get_global()->disks_hash_by_uuid,
					new_disk,
					config_tag,
					NVMEIBT_MAX_N_DISKS, disk, disk);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// In any case, update the following config-driven fields
	// None

out:
	if ((rv == NVMEIBT_ADD_NEW) || (rv == NVMEIBT_ADD_MODIFIED)) {
		if (rv == NVMEIBT_ADD_NEW)
			disk->urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&conf->uuid);
		if (nvmeibt_raft_is_leader() && nvmeibt_disk_is_explicitly_out_of_service(disk)) {
			// All disk's segments that are in X_ZERO state must be marked with X_DONE
			for (int i = 0; i < disk->n_segments; i++) {
				struct nvmeibt_disk_segment	*seg = disk->disk_segments[i];
				if (nvmeibt_disk_segment_is_x_zero(&seg->seg_follower.applied_seg_lot.seg_topo)) {
					N_Tf(gg8qxy5, "seg=@UUID_8", nvmeibt_seg_UUID_8(seg));
					NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(ko01n93, nvmeibt_disk_segment_get_praid(seg));
				}
			}
		}
	} else {
		N_Tf(trace_2_disk_nvmeibt_disk_add, "Freeing unused new ldisk=@STR", nvmeibt_disk_get_ldisk_id_str(new_disk));
		NNVMEIBT_BM_FREE(trace_3_disk_nvmeibt_disk_add, new_disk);
	}

	NFOUT;
	return rv;
}

bool nvmeibt_disk_is_local_in_config(const struct nvmeibt_disk *disk)
{
	return (disk && disk->its_node_config && disk->its_node_config->is_my_node);
}

bool nvmeibt_disk_is_local(const struct nvmeibt_disk *disk)
{
	return (disk && disk->its_local_disk);
}

void nvmeibt_disk_leader_detach_all_disks_from_raft_members(void)
{
	struct nvmeibt_disk			*disk;
	struct nvmeibt_raft_member	*member;

	NFIN;
	NVMEIB_HASH_FOREACH(disk, nvmeibt_global_get_global()->disks_hash_by_uuid) {
		disk->leader_its_raft_member = NULL;
		for (int i = 0; i < disk->n_segments; i++)
			nvmeibt_seg_remote_reset(&(disk->disk_segments[i]->seg_leader.remote_seg_topo), disk->disk_segments[i]);
	}
	XHASHTABLE_FOR_EACH_SAFE(member, &(my_raft_global.raft_members_hash)) {
		member->n_disks_leader = 0;
	}
	NFOUT;
}

void nvmeibt_disk_detach_from_node(struct nvmeibt_disk *disk)
{
	NFIN;
	N_Df(hfu4447, "disk=@UUID_LE", nvmeibt_disk_UUID(disk));
	nvmeibt_topology_remove_disk_from_its_current_node(disk);
	nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(disk);
	NFOUT;
}

int nvmeibt_disk_brute_force_del_all_segs_due_to_format(struct nvmeibt_disk *disk)
{
	struct nvmeibt_disk_segment					*seg;
	struct nvmeibt_disk_segment_topo_ctx		*applied_seg_topo;
	int											i;
	int											rv = 0;

	NFIN;
	if (!disk) {
		rv = 0;	// No disk_segment to disturb us, sort of.
		goto out;
	}
	for (i = disk->n_segments - 1; i >= 0; --i) {
		seg = disk->disk_segments[i];
		applied_seg_topo = &seg->seg_follower.applied_seg_lot.seg_topo;
		if (!nvmeibt_disk_segment_is_x(applied_seg_topo)) {
			N_Wf(hg7jwe3, "ldisk=@STR cannot be formatted: seg=@UUID_8 DIRTY_BITS=@STR",
				 nvmeibt_disk_get_ldisk_display(disk), nvmeibt_seg_UUID_8(seg), dirty_bits_state_str(applied_seg_topo->dirty_bits_state));
			rv = -1;
			goto out;
		}
	}
	for (i = disk->n_segments - 1; i >= 0; --i) {
		seg = disk->disk_segments[i];
		nvmeibt_seg_active_mark_as_fully_zeroed(nvmeibt_disk_segment_get_seg_active(seg));	// Delicate cheating for the remove
		if (nvmeibt_disk_segment_remove(seg) != NVMEIBT_SEG_REMOVED) {
			N_Wf(hg7jwd9, "Failed to remove seg=@UUID_8 ldisk=@STR", nvmeibt_seg_UUID_8(seg), nvmeibt_disk_get_ldisk_display(disk));
			rv = -1;
		}
	}
out:
	NFOUT;
	return rv;
}

void nvmeibt_disk_trim_unused_entries(int config_tag)
{
	struct nvmeibt_disk			*disk;
	int							seg_idx;

	NFIN;
	NVMEIB_HASH_FOREACH(disk, nvmeibt_global_get_global()->disks_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_OLDER(disk, config_tag)) {
			struct nvmeibt_local_disk *local_disk = disk->its_local_disk;

			N_Tf(dhuyr75, "drop disk=@UUID_LE with config tag @INT<@INT", nvmeibt_disk_UUID(disk), disk->config_tag, config_tag);

			nvmeibt_disk_detach_from_node(disk);

			for (seg_idx = disk->n_segments - 1; seg_idx >= 0; --seg_idx) {
				disk->disk_segments[seg_idx]->seg_mgmt.its_disk = NULL;
			}

			NNVMEIBT_TOMA_FREE(ki986ty, disk->disk_segments);
			disk->n_segments = 0;

			if (local_disk) {
				local_disk->its_disk = NULL;
				disk->its_local_disk = NULL;
				nvmeibt_topology_active_mark_reserialization_required();
			}

			NVMEIBT_OBJ_MARK_OUTDATED(fhuyt75, disk, disk);
			disk_remove(disk);
		}
	}

	NFOUT;
}

void nvmeibt_disk_free_all_at_exit(void)
{
	struct nvmeibt_disk			*disk;
	NVMEIB_HASH_FOREACH(disk, nvmeibt_global_get_global()->disks_hash_by_uuid) {
		NNVMEIBT_BM_FREE(vvtys8k, disk);
	}
}

int nvmeibt_disk_print_status_line(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_disk *disk, int n_prefix_tabs)
{
	int i;

	if (!disk) {
		goto out;
	}
	for (i = 0; i < n_prefix_tabs; i++) {
		(*printf_fn)(printf_ctx, "\t");
	}

	(*printf_fn)(printf_ctx, "- ldisk=%s disk=%s n_segments=%d config_node=%s\n",
				 nvmeibt_disk_get_ldisk_id_str(disk), nvmeibt_disk_id_str(disk), disk->n_segments, nvmeibt_node_name(disk->its_node_config));
out:
	return 0;
}

int nvmeibt_disk_print_disks_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_node		*node;
	struct nvmeibt_disk		*disk;
	int						j, k;

	(*printf_fn)(printf_ctx, "DISKS (%sleader)\n", (nvmeibt_raft_is_leader() ? "" : "NOTE: I am not the "));
	NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {
		(*printf_fn)(printf_ctx, "\t-%s%s%s\n", nvmeibt_node_name(node),
					 (node->is_my_node ? " (Me)" : ""), (nvmeibt_node_get_raft_member(node) ? "" : " - Not a member"));
		for (j = 0; j < node->n_disks_config; j++) {
			disk = node->disks_config[j];
			nvmeibt_disk_print_status_line(printf_fn, printf_ctx, disk, 2);
			for (k = 0; k < disk->n_segments; k++) {
				struct nvmeibt_disk_segment				*seg = disk->disk_segments[k];
				struct nvmeibt_disk_segment_topo_ctx	*topo_ctx;
				topo_ctx = (nvmeibt_raft_is_leader()? &(seg->seg_leader.baseline_seg_lot.seg_topo) : &(seg->seg_follower.applied_seg_lot.seg_topo));
				(*printf_fn)(printf_ctx, "\t\t\t- seg=%.8s vol=%s praid_ver=%x state=%s are_reg_sync=%d init_mode(dirty=%s stale=%s txid=%s) depr_flag=%c%s\n",
						nvmeibt_disk_segment_id_str(seg),
						nvmeibt_disk_segment_blkdev_name(seg),
						topo_ctx->seg_praid_version_major,
						dirty_bits_state_str(topo_ctx->dirty_bits_state),
						topo_ctx->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd,
						mem_tbl_init_mode_str(topo_ctx->dirty_bits_init_mode), mem_tbl_init_mode_str(topo_ctx->stale_locks_init_mode),
						mem_tbl_init_mode_str(topo_ctx->txid_init_mode),
						seg->from_config.deprecation_flag,
						nvmeibt_disk_segment_is_config_OK(seg) ? "" : " !is_config_OK");
			}

		}
	}
	(*printf_fn)(printf_ctx, "\t-Unknown node\n");
	NVMEIB_HASH_FOREACH(disk, nvmeibt_global_get_global()->disks_hash_by_uuid) {
		if ((nvmeibt_raft_is_leader() && !!(disk->leader_its_raft_member)) || !!(disk->its_node_config)) {
			continue;	// Skip disks with known node
		}
		nvmeibt_disk_print_status_line(printf_fn, printf_ctx, disk, 2);
		for (k = 0; k < disk->n_segments; k++) {
			struct nvmeibt_disk_segment				*seg = disk->disk_segments[k];
			struct nvmeibt_disk_segment_topo_ctx	*topo_ctx;
			topo_ctx = (nvmeibt_raft_is_leader()? &(seg->seg_leader.baseline_seg_lot.seg_topo) : &(seg->seg_follower.applied_seg_lot.seg_topo));
			(*printf_fn)(printf_ctx, "\t\t\t- seg=%.8s vol=%s praid_ver=%x state=%s are_reg_sync=%d init_mode(dirty=%s stale=%s txid=%s)\n",
					nvmeibt_disk_segment_id_str(seg),
					nvmeibt_disk_segment_blkdev_name(seg),
					topo_ctx->seg_praid_version_major,
					dirty_bits_state_str(topo_ctx->dirty_bits_state),
					topo_ctx->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd,
					mem_tbl_init_mode_str(topo_ctx->dirty_bits_init_mode), mem_tbl_init_mode_str(topo_ctx->stale_locks_init_mode),
					mem_tbl_init_mode_str(topo_ctx->txid_init_mode));
		}

	}
	return 0;
}

