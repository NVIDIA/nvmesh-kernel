#include "nvmeibt_block_device.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_seg_active.h"

BOOL nvmeibt_block_device_is_deprecated_in_config(struct nvmeibt_block_device *block_device)
{
	return (block_device && block_device->from_config.is_deprecated);
}

const union nvmeib_uuid *nvmeibt_block_device_UUID(struct nvmeibt_block_device *block_device)
{
	return (block_device ? &block_device->from_config.id : &nvmeib_uuid_null_val);
}

const char *nvmeibt_block_device_id_str(const struct nvmeibt_block_device *block_device)
{
	return (block_device ? block_device->urn_uuid.str : "");
}

void nvmeibt_block_device_dump(struct nvmeibt_block_device *block_device)
{
#ifdef TOMA_DEBUG
	struct nvmeibt_block_device_config *f = &block_device->from_config;

	N_Tf(vghs83o, "Config data: id=@UUID_LE version=@VERSION name=@NAME "
		 "n_chunks=@N_CHUNKS size=@SIZE_LLONG blksize=@INT attr={ver=@INT relative_rebuild_priority=@INT} is_deprecated=@BOOL kafka_offset=@INT64_TD",
		 &f->id, f->version, f->client_blkdev_name,
		 f->n_chunks, f->size_lblks, f->blk_size_bytes, f->attr.version, f->attr.relative_rebuild_priority, f->is_deprecated, f->mgmt_config_kafka_offset_or_idx);
#else	// #ifdef TOMA_DEBUG
	(void)block_device;
#endif	// #ifdef TOMA_DEBUG
}

static int block_device_remove(struct nvmeibt_block_device *block_device)
{
	int rv = -1;

	NFIN;
	if (block_device == NULL)
		goto out;

	N_Tf(fhu8236, "Removing block_device=@UUID_LE", nvmeibt_block_device_UUID(block_device));

	NNVMEIBT_HASH_DEL_OBJ(fhuu87w, &nvmeibt_global_get_global()->block_devices_hash, block_device, block_device);
	if (block_device->encrypt_params) { // Don't del the block_device if in the middle of encrypt operation
		block_device->encrypt_params = NULL;
		// NNVMEIBT_TOMA_FREE(skmnji2, block_device->encrypt_params->shadow_vol);
		// NNVMEIBT_TOMA_FREE(skmnjb9, block_device->encrypt_params);
	} else {
		NNVMEIBT_TOMA_FREE(skmnju7, block_device);
	}
	// The assumption is that the surrounding objects (block_device & chunk) are also removed

	rv = 0;

out:
	NFOUT;
	return rv;
}

static void blkdev_upd_config_tag_recursively(struct nvmeibt_block_device *blkdev, int config_tag)
{
	int								i, j, k;
	struct nvmeibt_chunk			*chunk;
	struct nvmeibt_praid			*praid;

	N_Tf(gob6vwz, "blkdev=@UUID_LE config_tag=@INT", nvmeibt_block_device_UUID(blkdev), config_tag);
	blkdev->config_tag = config_tag;
	for (i = 0; i < blkdev->n_chunks; i++) {
		chunk = blkdev->chunks[i];
		chunk->config_tag = config_tag;
		chunk->trim_flags &= ~CONFIG_TRIM_MGMT;
		for (j = 0; j < chunk->n_praids; j++) {
			praid = chunk->praids[j];
			praid->config_tag = config_tag;
			praid->trim_flags &= ~CONFIG_TRIM_MGMT;
			for (k = 0; k < praid->praid_mgmt.n_topo_segs; k++) {
				if (praid->praid_mgmt.topo_segs[k]) {
					praid->praid_mgmt.topo_segs[k]->config_tag = config_tag;
					praid->praid_mgmt.topo_segs[k]->trim_flags &= ~CONFIG_TRIM_MGMT;
				}
				if (praid->praid_mgmt.replacement_topo_segs[k]) {
					praid->praid_mgmt.replacement_topo_segs[k]->config_tag = config_tag;
					praid->praid_mgmt.replacement_topo_segs[k]->trim_flags &= ~CONFIG_TRIM_MGMT;
				}
			}
		}
	}
}

enum nvmeibt_add_rv nvmeibt_block_device_add(struct mm_vol_conf *vol, int config_tag, struct nvmeibt_block_device **output_block_device, bool is_topo_config)
{
	enum nvmeibt_add_rv					rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_block_device			*new_block_device = NULL;	// Read into it, maybe use it.
	struct nvmeibt_block_device			*block_device;
	struct nvmeibt_block_device_config	*f = NULL, prev_f;

	NFIN;
	//
	block_device = nvmeibt_block_device_get_block_device_by_id(&(vol->uuid));
	if (block_device) {
		block_device->trim_flags &= ~CONFIG_TRIM_MGMT;
		if (block_device->from_config.version >= (int)vol->version) {
			if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(block_device)) { // the vol is reanimated, take the new config
				N_Tf(ghu87b5, "blkdev=@UUID_LE version=@X was outdated, recreating", &(vol->uuid), vol->version);
				block_device->config_tag = config_tag;
				rv = NVMEIBT_ADD_MODIFIED;
				goto out;
			}
			N_Tf(r38nqlk, "blkdev=@UUID_LE blkdev->version=@X received version=@X, skipping", &(vol->uuid), block_device->from_config.version, vol->version);
			blkdev_upd_config_tag_recursively(block_device, config_tag);
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
			goto out;
		}
	}
	block_device = NULL;
	//
	new_block_device = NNVMEIBT_TOMA_CALLOC(ghu76dw, 1, sizeof *new_block_device);
	XDLIST_INIT_LINK(&new_block_device->topo_link, NULL);
	f = &(new_block_device->from_config);

	f->id = vol->uuid;
	f->version = vol->version;
	strncpy(f->client_blkdev_name, vol->name, sizeof(f->client_blkdev_name));
	f->n_chunks = vol->num_chunks;
	f->size_lblks = vol->blocks;
	f->blk_size_bytes = vol->blockSize;
	f->attr.version = f->version;
	f->attr.relative_rebuild_priority = vol->relativeRebuildPriority;
	f->is_deprecated = (vol->action == 'X');
	if (!is_topo_config) {
		f->mgmt_config_kafka_offset_or_idx = vol->kafka_offset_or_idx;
	}
	f->enableCrcCheck = vol->enableCrcCheck;
	f->use_debug_di = vol->use_debug_di;
	f->stripe_size = vol->stripeSize;
	f->stripe_width = vol->stripeWidth;

	rv = NNVMEIBT_HASH_ADD_OBJ(ti98dju,
							   &nvmeibt_global_get_global()->block_devices_hash,
							   new_block_device,
							   config_tag,
							   NVMEIBT_MAX_N_BLOCK_DEVICES,
							   block_device,
							   &prev_f,
							   block_device);
	if (rv == NVMEIBT_ADD_NEW) {
		block_device->urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&vol->uuid);
		block_device->trim_flags = 0;
		block_device->encrypt_idx = NVMEIBT_BLOCK_DEVICE_UNINITIALIZED_ENCRYPT_IDX;
		block_device->encrypt_params = NULL;
	} else {
		N_Tf(ty73932, "Freeing block_Device=@BLOCK_DEVICE_STR", nvmeibt_blkdev_name(new_block_device));
		NNVMEIBT_TOMA_FREE(ji9e8cf, new_block_device);
	}
	if (rv == NVMEIBT_ADD_NEW || rv == NVMEIBT_ADD_MODIFIED) {
		block_device->serialized_vol_conf = *vol;
	}
	block_device->n_chunks = block_device->from_config.n_chunks;

out:
	*output_block_device = block_device;
	NFOUT;
	return rv;
}

void nvmeibt_block_device_mark_conf_corrupted(struct nvmeibt_block_device *blkdev)
{
	N_Wf(warn_device_nvmeibt_block_device_mark_conf_corrupted, "blkdev=@UUID_LE", nvmeibt_block_device_UUID(blkdev));
	if (blkdev) {
		blkdev->is_conf_corrupted = 1;
		nvmeibt_mark_conf_corrupted();
	}
}

struct nvmeibt_block_device *nvmeibt_block_device_get_block_device_by_id(const union nvmeib_uuid *block_device_id)
{
	return NNVMEIBT_HASH_GET_OBJ_BY_UUID(bhwila4, &nvmeibt_global_get_global()->block_devices_hash, block_device_id, block_device);
}

/* Check if can do garbage collection on a candidate block device */
BOOL nvmeibt_block_device_may_garbage_collect(struct nvmeibt_block_device *block_device)
{
	int								block_dev_chunk_ind, chunk_praid_ind;
	BOOL							ret = 0;
	struct nvmeibt_disk_segment		*disk_segment;

	NFIN;
	// Avoid if any of its disk_segments is not deletable
	for (block_dev_chunk_ind = block_device->n_chunks - 1; block_dev_chunk_ind >= 0; --block_dev_chunk_ind) {
		struct nvmeibt_chunk	*chunk = block_device->chunks[block_dev_chunk_ind];
		if (!chunk) {
			N_Tf(gdyy733, "@BLKDEV_NAME->chunk[@BLOCK_DEV_CHUNK_IND]=NULL", nvmeibt_blkdev_name(block_device), block_dev_chunk_ind);
			continue;
		}
		for (chunk_praid_ind = chunk->n_praids - 1; chunk_praid_ind >= 0; --chunk_praid_ind) {
			struct nvmeibt_praid	*praid = chunk->praids[chunk_praid_ind];
			if (!praid) {
				N_Tf(gkoiu87, "@UUID_LE->praids[@CHUNK_PRAID_IND]=NULL", nvmeibt_chunk_UUID(chunk), chunk_praid_ind);
				continue;
			}
			if (nvmeibt_praid_get_n_recoveries_needing_hidden_attach(praid)) {
				N_Tf(93iksp2, "Not yet. praid=@UUID_LE n_recoveries_needing_hidden_attach=@INT", nvmeibt_praid_UUID(praid), nvmeibt_praid_get_n_recoveries_needing_hidden_attach(praid));
				goto out;
			}
			XDLIST_FOREACH(disk_segment, &praid->praid_mgmt.all_segs_list) {
				if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(disk_segment)) {
					N_Tf(rr998vs, "Not yet. Awaiting seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
					goto out;
				}
			}
		}
	}
	ret = 1;
out:
	NFOUT;
	return ret;
}

/*
 * Try to do garbage collection of block devices. Returns:
 *   1  : at least one (segment/chunk of one) entry removed
 *   0  : no entries removed
 */
void nvmeibt_block_devices_garbage_collect(bool *is_any_garbage_collected, bool *is_all_garbage_collected)
{
	struct nvmeibt_block_device	*block_device;
	struct nvmeibt_block_device	*blkdev_for_GC = NULL;
	struct nvmeibt_chunk		*chunk;
	struct nvmeibt_praid		*praid;
	struct nvmeibt_disk_segment	*disk_segment;
	int							n_blkdevs_needing_garbage_collection = 0;
	int							i, j;
	bool						is_praid_removable;
	bool						is_chunk_removable;
	bool						is_blkdev_removable;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	NFIN;
	*is_any_garbage_collected = 0;
	// If some block_device is_being_deleted, then clean it if possible.
	// (In pre-history it was done one at a time, so keep the tradition)
	XHASHTABLE_FOR_EACH_SAFE(block_device, &cur_topo->block_devices_hash) {
		if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(block_device))
			continue;
		n_blkdevs_needing_garbage_collection += 1;
		if (blkdev_for_GC || !nvmeibt_block_device_may_garbage_collect(block_device))
			continue;
		blkdev_for_GC = block_device;
	}
	if (!blkdev_for_GC) {
		goto out;
	}
	// Garbage_collect the single blkdev found
	is_blkdev_removable = 1;
	for (i = blkdev_for_GC->n_chunks - 1; i >= 0; --i) {
		chunk = blkdev_for_GC->chunks[i];
		if (!chunk) {
			continue;
		}
		is_chunk_removable = 1;
		for (j = chunk->n_praids - 1; j >= 0; j--) {
			praid = chunk->praids[j];
			if (!praid) {
				continue;
			}
			is_praid_removable = 1;
			XDLIST_FOREACH(disk_segment, &praid->praid_mgmt.all_segs_list) {
				// Not removing segs, happened before the call to this func.
				N_Tf(t_h1_tmbdv, "praid=@UUID_LE has seg=@UUID_8. Cannot remove blkdev", nvmeibt_praid_UUID(praid), nvmeibt_seg_UUID_8(disk_segment));
				is_praid_removable = 0;
			}
			if (is_praid_removable && (nvmeibt_praid_remove(chunk->praids[j]) == 0)) {
				*is_any_garbage_collected = 1;
			} else {
				is_chunk_removable = 0;
			}
		}
		if (is_chunk_removable && (nvmeibt_chunk_remove(chunk) == 0)) {
			*is_any_garbage_collected = 1;
		} else {
			is_blkdev_removable = 0;
		}
	}
	if (is_blkdev_removable) {
		if (block_device_remove(blkdev_for_GC) == 0) {
			*is_any_garbage_collected = 1;
			n_blkdevs_needing_garbage_collection -= 1;
		}
	}
out:
	*is_all_garbage_collected = (n_blkdevs_needing_garbage_collection == 0);
	NFOUT;
}

void nvmeibt_block_device_trim_specific_block_device(struct nvmeibt_block_device *block_device, uint8_t trim_flag)
{
	if (is_trim_needed(&block_device->trim_flags, trim_flag)) {
		NVMEIBT_HASH_MARK_OBJ_OUTDATED(fhu8772, block_device, block_device);
		NNVMEIBT_BUF_FREE(cvajeb5, &(block_device->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf));
	}
	else {
		N_Tf(huhfy77, "not yet, vol=@UUID_LE flags=@X", nvmeibt_block_device_UUID(block_device), block_device->trim_flags);
	}
}

void nvmeibt_block_device_trim_unused_entries(int config_tag, uint8_t trim_flag)
{
	struct nvmeibt_block_device		*block_device;
	int64_t							committed_idx = nvmeibt_global_get_global()->highest_seen_committed_kafka_mgmt_config_idx;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(block_device, &nvmeibt_global_get_global()->block_devices_hash) {
		if (NVMEIBT_HASH_IS_OLDER_OBJ(block_device, config_tag)) {
			if (block_device->from_config.mgmt_config_kafka_offset_or_idx > committed_idx) {
				N_IMf(gy7887t, "vol=@STR is newer than committed cfg. (vol_kafka_offset=@INT64, committed_kafka_offset=@INT64)",
					  block_device->from_config.client_blkdev_name, block_device->from_config.mgmt_config_kafka_offset_or_idx, committed_idx);
			}
			nvmeibt_block_device_trim_specific_block_device(block_device, trim_flag);
		}
	}
	NFOUT;
}

int nvmeibt_block_device_validate_blkdevs_config(void)
{
	struct nvmeibt_block_device	*blkdev;
	long long	min_lblk;
	long long	max_lblk;
	long long	sum;
	int			j, k;
	int			rv = 0;
	struct nvmeibt_chunk			*chunk0;
	struct nvmeibt_chunk			*chunk1;
	struct nvmeibt_chunk_config 	*f0;
	struct nvmeibt_chunk_config 	*f1;
	NFIN;

	XHASHTABLE_FOR_EACH_SAFE(blkdev, &nvmeibt_global_get_global()->block_devices_hash) {
		min_lblk = LLONG_MAX;
		max_lblk = LLONG_MIN;
		sum = 0;
		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk0 = blkdev->chunks[j];
			f0 = &(chunk0->from_config);
			N_Tf(kffi220s, "@UUID_LE vlb_s=@LLD vlb_e=@LLD", &f0->id, f0->vlb_s, f0->vlb_e);
			min_lblk = min(min_lblk, f0->vlb_s);
			max_lblk = max(max_lblk, f0->vlb_e);
			sum += (f0->vlb_e - f0->vlb_s + 1);
			for (k = j + 1; k < blkdev->n_chunks; k++) {
				chunk1 = blkdev->chunks[k];
				f1 = &(chunk1->from_config);
				if (nvmeibt_do_ranges_overlap(f0->vlb_s, f0->vlb_e, f1->vlb_s, f1->vlb_e)) {
					N_Ef(uee3811, "@DEVICE_NAME: blkset_allocations overlap @UUID_LE @LLD-@LLD, @UUID_LE @LLD-@LLD",
						nvmeibt_blkdev_name(blkdev), &f0->id, f0->vlb_s, f0->vlb_e, &f1->id, f1->vlb_s, f1->vlb_e);
					nvmeibt_chunk_mark_conf_corrupted(chunk0);
					nvmeibt_chunk_mark_conf_corrupted(chunk1);
					nvmeibt_block_device_mark_conf_corrupted(blkdev);
				}
			}
		}
		if (	sum != blkdev->from_config.size_lblks ||
				min_lblk != 0 ||
				max_lblk != (blkdev->from_config.size_lblks - 1)) {
			N_Ef(error_1_device_nvmeibt_block_device_validate_blkdevs_config, "@DEVICE_NAME size=@LLD sum=@LLD min_lblk=@LLD max_lblk=@LLD", blkdev->from_config.client_blkdev_name, blkdev->from_config.size_lblks, sum, min_lblk, max_lblk);
			nvmeibt_block_device_mark_conf_corrupted(blkdev);
		}
		else {
			N_Tf(trace_1_device_nvmeibt_block_device_validate_blkdevs_config, "Validated blkdev=@BLKDEV size=@SIZE_LLONG", blkdev->from_config.client_blkdev_name, blkdev->from_config.size_lblks);
		}
	}
	NFOUT_rv;
	return rv;
}

void nvmeibt_block_device_reservation_mode_change(const union nvmeib_uuid *vol_uuid, uint64_t reservation_version)
{
	int									j, k, i;
	struct nvmeibt_block_device			*blkdev;
	struct nvmeibt_chunk				*chunk;
	struct nvmeibt_praid				*praid;
	struct nvmeibt_seg_lot				*seg_lot;
	struct nvmeibt_seg_active			*seg_active;

	NFIN;
	blkdev = nvmeibt_block_device_get_block_device_by_id(vol_uuid);
	if (blkdev) {
		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk = blkdev->chunks[j];
			for (k = 0; k < chunk->n_praids; k++) {
				praid = chunk->praids[k];
				for (i = 0; i < praid->praid_follower.applied_praid_lot.n_topo_seg_lots; i++) {
					seg_lot = praid->praid_follower.applied_praid_lot.topo_seg_lots[i];
					seg_active = nvmeibt_disk_segment_get_seg_active(seg_lot->my_seg);
					nvmeibt_register_handle_new_reservation_mode(seg_active, reservation_version);
				}
			}
		}
	} else {
		N_Wf(ju8876x, "Could not find blkdev=@UUID_LE", vol_uuid);
	}
	NFOUT;
}


/*********    Print Status   *************/

int nvmeibt_block_device_print_blkdevs_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_block_device	*blkdev;
	int							j, k;

	(*printf_fn)(printf_ctx, "BLOCK DEVICES (%sleader)\n", (nvmeibt_raft_is_leader() ? "" : "NOTE: For reliable results go to the "));
	if (XHASHTABLE_N_ELEMENTS(&nvmeibt_global_get_global()->block_devices_hash) == 0)
		return 0;
	XHASHTABLE_FOR_EACH_SAFE(blkdev, &nvmeibt_global_get_global()->block_devices_hash) {
		(*printf_fn)(printf_ctx, "\t- Volume=%s\tVersion=%d nChunks=%d\n",
				blkdev->from_config.client_blkdev_name, blkdev->from_config.version, blkdev->from_config.n_chunks);
		for (j = 0; j < blkdev->n_chunks; j++) {
			struct nvmeibt_chunk	*chunk = blkdev->chunks[j];
			(*printf_fn)(printf_ctx, "\t\t- Chunk=%s Start_LBA=%llx End_LBA=%llx\n",
				nvmeibt_chunk_id_str(chunk),
				chunk->from_config.vlb_s,
				chunk->from_config.vlb_e);
			for (k = 0; k < chunk->n_praids; k++) {
				nvmeibt_praid_print_status(printf_fn, printf_ctx, chunk->praids[k]);
			}
		}
	}
	return 0;
}


const char *zeroing_state_str(enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE s)
{
	switch (s) {
	case NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO: return "In progress";
	case NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE: return "Finished";
	default : return "Not in zeroing";
	}
}

void nvmeibt_block_device_print_zeroing_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_block_device					*blkdev;
	int											j, k;
	struct nvmeibt_chunk						*chunk;
	struct nvmeibt_praid 						*praid = NULL;
	struct nvmeibt_praid_lot					*praid_lot;
	struct nvmeibt_disk_segment					*disk_segment;
	struct nvmeibt_seg_lot						*seg_lot;
	struct nvmeibt_seg_active 					*seg_active;
	struct nvmeibt_disk_segment_topo_ctx 		*seg_topo_ctx;

	(*printf_fn)(printf_ctx, "VOLUMES ZEROING\n");
	XHASHTABLE_FOR_EACH_SAFE(blkdev, &nvmeibt_global_get_global()->block_devices_hash) {
		(*printf_fn)(printf_ctx, "\t- Volume=%s Version=%d nChunks=%d\n",
				blkdev->from_config.client_blkdev_name, blkdev->from_config.version, blkdev->from_config.n_chunks);
		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk = blkdev->chunks[j];
			for (k = 0; k < chunk->n_praids; k++) {
				praid = chunk->praids[k];
				praid_lot = &praid->praid_follower.applied_praid_lot;
				XDLIST_FOREACH(seg_lot, &praid_lot->all_seg_lot_list) {
					seg_topo_ctx = &seg_lot->seg_topo;
					if (nvmeibt_disk_segment_is_x(seg_topo_ctx))
						goto print_status;
				}
			}
		}

		(*printf_fn)(printf_ctx, "\t\t No zeroing for the volume\n");
    	continue;

print_status:

		for (j = 0; j < blkdev->n_chunks; j++) {
			chunk = blkdev->chunks[j];
			(*printf_fn)(printf_ctx, "\t\t- Chunk=%s Start_LBA=%llx End_LBA=%llx\n",
				nvmeibt_chunk_id_str(chunk),
				chunk->from_config.vlb_s,
				chunk->from_config.vlb_e);
			for (k = 0; k < chunk->n_praids; k++) {
				praid = chunk->praids[k];
				praid_lot = &praid->praid_follower.applied_praid_lot;
				(*printf_fn)(printf_ctx, "\t\t\t- praid=%s type=%s\n", nvmeibt_praid_id_str(praid), nvmeibt_praid_type_str(praid->praid_mgmt.type));
				XDLIST_FOREACH(seg_lot, &praid_lot->all_seg_lot_list) {
					disk_segment = seg_lot->my_seg;
					seg_topo_ctx = &seg_lot->seg_topo;
					(*printf_fn)(printf_ctx, "\t\t\t\t- seg=%.8s node=%s zeroing state=%s",
								 nvmeibt_disk_segment_id_str(disk_segment),
								 nvmeibt_raft_is_leader() ? nvmeibt_disk_get_leader_node_name(disk_segment->seg_mgmt.its_disk) :
															nvmeibt_disk_get_applied_node_name(disk_segment->seg_mgmt.its_disk),
								 zeroing_state_str(seg_topo_ctx->dirty_bits_state));
    				if (nvmeibt_disk_segment_is_x_zero(seg_topo_ctx)) {
						seg_active = nvmeibt_disk_segment_get_seg_active(disk_segment);
    					if (seg_active) {
							(*printf_fn)(printf_ctx, " %lld 4K blocks out of %lld 4K blocks zeroed\n",
										 seg_active->n_4Kblk_zeroed,
										 (disk_segment->seg_mgmt.lb_e - disk_segment->seg_mgmt.lb_s + 1));
						}
    					else {
							(*printf_fn)(printf_ctx, " (Not local machine)\n");
						}
					}
    				else {
						(*printf_fn)(printf_ctx, "\n");
					}
				}
			}
		}
	}
}

