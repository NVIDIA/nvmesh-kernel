#include "nvmeibt_disk_segment.h"
#include "nvmeibt_praid.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_recovery.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_topo_bin.h"
#include "../common/nvmeib_hash.h"


struct nvmeibt_local_disk *nvmeibt_disk_segment_get_local_disk(struct nvmeibt_disk_segment *seg)
{
	// Called only for a local seg (that has a local_disk)
	return nvmeibt_seg_active_get_local_disk(nvmeibt_disk_segment_get_seg_active(seg));
}

bool nvmeibt_disk_segment_is_node_leader_valid(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_disk		*its_disk = seg_lot->my_seg->seg_mgmt.its_disk;

	return (its_disk && nvmeibt_raft_leader_is_peer_vote_recent(its_disk->leader_its_raft_member));
}

bool nvmeibt_disk_segment_leader_is_init_due_now(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_disk_segment_topo_ctx 		*seg_topo = &seg_lot->seg_topo;

	return (!nvmeibt_disk_segment_is_mem_tbl_init_done_fully(seg_topo) &&
			nvmeibt_disk_segment_leader_is_state_progressible(seg_topo) &&
			nvmeibt_disk_segment_is_node_leader_valid(seg_lot));
}

bool nvmeibt_disk_segment_leader_is_new_dead(struct nvmeibt_disk_segment *disk_segment)
{
	return (!nvmeibt_disk_segment_is_dirty_bits_state_registrable(disk_segment->seg_leader.calculated_seg_lot.seg_topo.dirty_bits_state) &&
			 nvmeibt_disk_segment_is_dirty_bits_state_registrable(disk_segment->seg_leader.baseline_seg_lot.seg_topo.dirty_bits_state));
}

bool nvmeibt_disk_segment_leader_is_waiting_for_dirty_bits_init(const struct nvmeibt_disk_segment *disk_segment)
{
	// Waiting only on usable segs (otherwise filtered elsewhere). Already based on remote applied
	return (nvmeibt_disk_segment_leader_is_state_progressible(&(disk_segment->seg_leader.calculated_seg_lot.seg_topo)) &&
			disk_segment->seg_leader.calculated_seg_lot.seg_topo.dirty_bits_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
}

BOOL nvmeibt_disk_segment_leader_is_waiting_for_stale_locks_init(const struct nvmeibt_disk_segment *disk_segment)
{
	// Waiting only on usable segs (otherwise filtered elsewhere). Already based on remote applied
	return (nvmeibt_disk_segment_leader_is_state_progressible(&(disk_segment->seg_leader.calculated_seg_lot.seg_topo)) &&
			disk_segment->seg_leader.calculated_seg_lot.seg_topo.stale_locks_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
}

#ifdef TOMA_DEBUG
void nvmeibt_disk_segment_dump(const struct nvmeibt_disk_segment *seg)
{
	const struct nvmeibt_disk_segment_config *f = &seg->from_config;
	N_Tf(ghfyr77, "Config data: id=@UUID_LE version=@VERSION praid=@UUID_LE disk=@UUID_LE "
		"lb_s=@LB_S lb_e=@LB_E idx_in_praid=@IDX_IN_PRAID deprecation_flag=@DEPRECATION_FLAG",
		&f->id, f->version, &seg->seg_mgmt.praid_id, &seg->seg_mgmt.disk_id,
		seg->seg_mgmt.lb_s, seg->seg_mgmt.lb_e, f->idx_in_praid, f->deprecation_flag);
}
#else	// #ifdef TOMA_DEBUG
void nvmeibt_disk_segment_dump(__attribute__((__unused__)) const struct nvmeibt_disk_segment *disk_segment) {}
#endif	// #ifdef TOMA_DEBUG

void nvmeibt_seg_lot_set_all_init_modes(struct nvmeibt_seg_lot *seg_lot, enum NVMEIBT_MEM_TBL_INIT_MODE init_mode)
{
	NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE( bgy61na, seg_lot, init_mode);
	NNVMEIBT_SEG_LOT_SET_TXID_INIT_MODE(       i9k9ks2, seg_lot, init_mode);
	NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(asdfc98, seg_lot, init_mode);
}

void nvmeibt_disk_segment_mark_is_newly_added_seg_in_all_topos(struct nvmeibt_disk_segment *seg)
{
	seg->seg_leader.baseline_seg_lot.seg_topo.leader_seg_flags.is_newly_added_seg = 1;
	seg->seg_leader.calculated_seg_lot.seg_topo.leader_seg_flags.is_newly_added_seg = 1;
	seg->seg_leader.to_report_seg_lot.seg_topo.leader_seg_flags.is_newly_added_seg = 1;
	seg->seg_follower.committed_seg_lot.seg_topo.leader_seg_flags.is_newly_added_seg = 1;
	seg->seg_follower.applied_seg_lot.seg_topo.leader_seg_flags.is_newly_added_seg = 1;
}

void nvmeibt_generic_seg_topo_reset(struct nvmeibt_disk_segment_topo_ctx *seg_topo)
{
	seg_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 1;	// No sync in the air for praid_version==0
	seg_topo->is_registrants_synchronizer = 0;
	seg_topo->seg_praid_version_major = PRAID_VERSION_INVALID_VALUE;
	seg_topo->seg_praid_version_minor = PRAID_VERSION_INVALID_VALUE;
	seg_topo->active_seg_ser_ver = NVMEIBT_NOT_INITIALIZED_SER_VER;
}

void nvmeibt_seg_remote_reset(struct nvmeibt_disk_segment_topo_ctx *seg_remote_topo, struct nvmeibt_disk_segment *seg)
{
	struct nvmeibt_seg_leader		*seg_leader = &(seg->seg_leader);

	TODO(WHAT IS THIS? seg_leader->remote_seg_topo = seg->seg_follower.committed_seg_lot.seg_topo;);
	nvmeibt_generic_seg_topo_reset(seg_remote_topo);
	seg_leader->is_removed_from_remote_applied = 0;
	//
	NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(txvjauk, seg_leader, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
	NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS_INIT_MODE(tvsjh83, seg_leader, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	NNVMEIBT_SEG_REMOTE_SET_TXID_INIT_MODE(zmcint5, seg_leader, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	NNVMEIBT_SEG_REMOTE_SET_STALE_LOCKS_INIT_MODE(xvzy823, seg_leader, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
}

static void new_seg_lot_init(struct nvmeibt_seg_lot *seg_lot, struct nvmeibt_disk_segment *seg)
{
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo;

	seg_topo = &seg_lot->seg_topo;
	seg_lot->my_seg = seg;
	seg_lot->from_config.version = ILLEGAL_CONFIG_VER;
	XDLIST_INIT_LINK(&(seg_lot->praid_all_seg_lots_link), NULL);
	nvmeibt_generic_seg_topo_reset(seg_topo);
	// seg topo
	NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(cba4hq8, seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
	nvmeibt_seg_lot_set_all_init_modes(seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
}

static void build_seg_from_config(struct nvmeibt_disk_segment_config *f, struct mm_segment_conf *conf, struct mm_vol_conf *vol)
{
	f->id = conf->uuid;
	f->idx_in_praid = conf->pRaidIndex;
	//f->idx_in_praid_role = conf->pRaidTypeIndex;
	f->deprecation_flag = conf->action;
	if (vol->action == 'X')
		f->deprecation_flag = vol->action;
}

enum nvmeibt_add_rv nvmeibt_disk_segment_add(struct mm_segment_conf *conf,
											 struct mm_praid_conf *praid,
											 struct mm_vol_conf *vol,
											 bool is_updating_leader,
											 struct nvmeibt_disk_segment **seg_out,
											 int config_tag)
{
	enum nvmeibt_add_rv						rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_disk_segment				*new_disk_segment = NULL;	// Read into it, maybe use it.
	struct nvmeibt_disk_segment				*disk_segment;
	struct nvmeibt_disk_segment_config		*f;
	struct nvmeibt_topology					*cur_topo = nvmeibt_global_get_global();

	NFIN;

	disk_segment = nvmeibt_disk_segment_get_disk_segment_by_id(&(conf->uuid));
	if (disk_segment) {
		disk_segment->trim_flags &= ~CONFIG_TRIM_MGMT;
		if (disk_segment->from_config.version >= (int)vol->version) {
			N_Tf(ycvbsk2, "seg=@UUID_LE seg->version=@X received version=@X, skipping", &(conf->uuid), disk_segment->from_config.version, vol->version);
			disk_segment->config_tag = config_tag;
			disk_segment->seg_follower.is_modified_in_last_config = 0;
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
			goto out;
		}
	}
	disk_segment = NULL;

	new_disk_segment = NNVMEIBT_BM_CALLOC(fh57uw2, sizeof *new_disk_segment);

	f = &(new_disk_segment->from_config);
	build_seg_from_config(f, conf, vol);
	f->version = vol->version; // In MGMT config take this field from vol

	if (conf->type == 0) {
		N_Wf(juq762b, "Obsolete raft_only seg=@UUID_LE found. Skipping", &f->id);
		rv = NVMEIBT_ADD_SKIPPED;
		goto out;
	}
	if ((f->idx_in_praid < 0) || (f->idx_in_praid >= NVMEIBT_MAX_N_SEGMENTS_IN_PRAID)) {
		N_Ef(yt675r4, "seg=@UUID_LE idx_in_praid=@IDX_IN_PRAID >= @_12", &f->id,
			f->idx_in_praid, NVMEIBT_MAX_N_SEGMENTS_IN_PRAID);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}
	if (conf->lbs % NUM_4KBLKS_IN_BLKSET || (conf->lbe + 1) % NUM_4KBLKS_IN_BLKSET) {
		N_Ef(acki98u, "Disk segment's start and end block must be aligned to @_32", NUM_4KBLKS_IN_BLKSET);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}

	rv = NNVMEIBT_HASH_ADD_OBJ_new(uy7uy43,
							   cur_topo->disk_segments_hash_by_uuid,
							   new_disk_segment,
							   config_tag,
							   NVMEIBT_MAX_N_DISK_SEGMENTS,
							   disk_segment, NULL, seg);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	if ((rv == NVMEIBT_ADD_NEW) || (rv == NVMEIBT_ADD_MODIFIED)) {
		disk_segment->seg_follower.is_modified_in_last_config = 1;
		if (rv == NVMEIBT_ADD_NEW) {
			disk_segment->seg_mgmt.urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&conf->uuid);
			disk_segment->seg_mgmt.praid_id = praid->uuid;
			disk_segment->seg_mgmt.disk_id = conf->diskUUID;
			disk_segment->seg_mgmt.lb_s = conf->lbs;
			disk_segment->seg_mgmt.lb_e = conf->lbe;
			// Init the leader seg_lots
			new_seg_lot_init(&disk_segment->seg_leader.baseline_seg_lot, disk_segment);
			new_seg_lot_init(&disk_segment->seg_leader.calculated_seg_lot, disk_segment);
			new_seg_lot_init(&disk_segment->seg_leader.to_report_seg_lot, disk_segment);
			//
			new_seg_lot_init(&disk_segment->seg_follower.committed_seg_lot, disk_segment);
			new_seg_lot_init(&disk_segment->seg_follower.applied_seg_lot, disk_segment);
			nvmeibt_seg_remote_reset(&(disk_segment->seg_leader.remote_seg_topo), disk_segment);
			XDLIST_INIT_LINK(&disk_segment->praid_all_segs_link, NULL);
			disk_segment->trim_flags = 0;
		}
		if (is_updating_leader)
			NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(uy7ndu1, disk_segment->seg_mgmt.its_praid);

	} else {
		disk_segment->seg_follower.is_modified_in_last_config = 0;
	}

out:
	if (rv == NVMEIBT_ADD_NEW) {
		/* nothing */ ;
	} else {
		N_Tf(ry76ty5, "Freeing unused seg");
		NNVMEIBT_BM_FREE(chj09s3, new_disk_segment);
	}

	*seg_out = disk_segment;
	NFOUT;
	return rv;
}

void nvmeibt_seg_update_committed_lot_config(struct mm_segment_conf *conf,
											 struct mm_praid_conf *praid_conf,
											 struct mm_vol_conf *vol,
											 struct nvmeibt_praid *praid,
											 struct nvmeibt_disk_segment **seg_out)
{
	struct nvmeibt_disk_segment					*seg;

	seg = nvmeibt_disk_segment_get_disk_segment_by_id(&conf->uuid);
	if (seg) {
		build_seg_from_config(&seg->seg_follower.committed_seg_lot.from_config, conf, vol);
	} else { // seg doesn't exist in MGMT config, but still exists in TOPO config
		if (vol->action == 'X')
			goto out;
		N_Tf(y7782ws, "add deleted in MGMT config seg=@UUID_LE", &conf->uuid);
		if (nvmeibt_disk_segment_add(conf, praid_conf, vol, 0, &seg, 1) != NVMEIBT_ADD_NEW) {
			N_Ef(gaay22w, "Cannot add old segment");
			nvmeibt_abort(ES_FATAL);
		}
		//Mark the seg as "can be removed from the point of view MGMT"
		seg->trim_flags = CONFIG_TRIM_MGMT;
		nvmeibt_read_config_add_missing_seg_to_praid(praid, seg);
	}

	seg->seg_follower.committed_seg_lot.from_config.version = praid_conf->version; // In TOPO config take this field from praid
	N_Tf(gyu1712, "seg=@UUID_8 committed", nvmeibt_seg_UUID_8(seg));

out:
	*seg_out = seg;
}

struct nvmeibt_disk_segment *nvmeibt_disk_segment_get_disk_segment_by_id(const union nvmeib_uuid *id)
{
	struct nvmeibt_disk_segment 	*disk_segment;

	disk_segment = nvmeib_hash_search_uuid(nvmeibt_global_get_global()->disk_segments_hash_by_uuid, id);

	if (disk_segment == NULL) {
		N_Tf(p0o9w72, "Segment not found id=@UUID_LE", id);
	}

	return disk_segment;
}

/***********************                              ************************/

void nvmeibt_seg_lot_leader_convert_unusable_to_dead(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo = &seg_lot->seg_topo;

	NFIN;
	N_Tf(gt654q2, "seg=@UUID_8 @DIRTY_BITS_STATE_STR is-(dep=@DEP rep=@REP)", nvmeibt_seg_lot_UUID_8(seg_lot),
		 dirty_bits_state_str(seg_topo->dirty_bits_state), seg_lot->from_config.deprecation_flag, seg_lot->is_replacement);
	if ((seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN) &&
		(seg_lot->is_replacement) &&
		!nvmeibt_disk_segment_is_dirty_bits_state_calculated(seg_lot_get_baseline_seg_topo(seg_lot)->dirty_bits_state))
	{
		// replacement in config, but not alive, and did not already replace
		goto out;	// Leave as is
	}
	if ((!nvmeibt_disk_segment_is_node_leader_valid(seg_lot) || !nvmeibt_disk_segment_leader_is_state_progressible(seg_topo)) &&
		!nvmeibt_seg_lot_is_deprecated_in_config(seg_lot)) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(cxvbnfq, seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD);
		nvmeibt_seg_lot_set_all_init_modes(seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
	}
out:
	NFOUT;
}

void nvmeibt_disk_segment_leader_sync_with_remote_applied(struct nvmeibt_seg_lot *calculated_seg_lot)
{
	bool									is_remote_active_fully_synched;
	struct nvmeibt_disk_segment 			*disk_segment = calculated_seg_lot->my_seg;
	struct nvmeibt_praid					*praid = disk_segment->seg_mgmt.its_praid;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo = &calculated_seg_lot->seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
	int										SEG_UUID_8 = nvmeibt_seg_lot_UUID_8(calculated_seg_lot);
	enum PRAID_REGISTRANTS_SYNC_CMD			sync_cmd = calculated_seg_lot->praid_lot->topo_ctx.registrants_sync_cmd;

	NFIN;
	// If a calculated and remote topos are not fully synched it means, that we decided to move forward without the seg.
	// Thus, mark the seg as a kind of dead and take nothing from remote.
	// The only exception is not was_praid_ever_activated, that treated separately
	is_remote_active_fully_synched = nvmeibt_seg_lot_is_remote_active_fully_synched(calculated_seg_topo, remote_seg_topo);
	if (nvmeibt_seg_lot_is_deleted_in_config(calculated_seg_lot)) {
		goto out;
	}
	// If already X (fully deprecated) or not_activated then there is nothing to learn from remote applied
	TODO(Is the next if possible following the prev if is_deleted_in_config);
	if (nvmeibt_disk_segment_is_x_done(calculated_seg_topo) ||
		nvmeibt_disk_segment_is_x_done(remote_seg_topo) ||
		(!praid->was_praid_ever_activated && nvmeibt_seg_lot_is_zeroing_explicitly_required_according_to_config(calculated_seg_lot))) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(shertnq, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE);
		N_Tf(fgyrhy3, "seg=@UUID_8 state=@DIRTY_BITS_STATE_STR activated=@IS_ACTIVATED deprecated=@DEPRECATED",
			 SEG_UUID_8, dirty_bits_state_str(calculated_seg_topo->dirty_bits_state), praid->was_praid_ever_activated, calculated_seg_lot->from_config.deprecation_flag);
		goto out;
	}
	// If not alive-and-relevant (praid_version wise) on remote-TOMA
	if (	(!is_remote_active_fully_synched && praid->was_praid_ever_activated) ||
			!nvmeibt_disk_segment_leader_is_state_progressible(remote_seg_topo) ||
            !nvmeibt_disk_segment_is_node_leader_valid(calculated_seg_lot)) {
		N_Tf(u87u76y, "seg=@UUID_8 is_synched=@INT remote_state=@REMOTE_STATE peer_node=@PEER_NODE",
			 SEG_UUID_8,
			 is_remote_active_fully_synched,
			 dirty_bits_state_str(remote_seg_topo->dirty_bits_state),
			 nvmeibt_disk_get_leader_node_name(nvmeibt_disk_segment_get_disk(disk_segment)));

		// If not already marked as some kind of dead then copy from the remote applied
		if (nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo)) {
			if (nvmeibt_disk_segment_leader_is_state_progressible(remote_seg_topo)) {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(trace_zzz_2, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN);
				NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(trace_zzz_3, &disk_segment->seg_leader, NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN); // For vitality report to mgmt
			}
			else {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(trace_zzz_1, calculated_seg_lot, remote_seg_topo->dirty_bits_state);
			}
		}
		goto out;
	}
	//
	// The disk_segment is alive!
	// is_remote_active_fully_synched==true which implies one of:     (also might get here if was never activated)
	// 1. All is good, stayed alive, with valid memory
	// 2. Went stable shutdown, saved the version, and now ALIVE_STABLE
	// 3. ALIVE_UNSTABLE, and the peer got our latest global topo. It went down, and awaiting init etc.
	//

	// If first time ever for the entire PRAID
	if (!(praid->was_praid_ever_activated)) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(hgt654e, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE);
		NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(xcvft54, &disk_segment->seg_leader, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		goto consider_accepting_remote_INIT_MODE;
	}
	// We get here only if is_remote_active_fully_synched==true and was_praid_ever_activated==true.
	NTOMA_ASSERT(dnvu2bo, is_remote_active_fully_synched, "is_remote_active_fully_synched=false");
	// If back from dead (in topology) to alive
	if (!nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo)) {
		// Don't revive additional segments during cold or dirty recovery
		if (nvmeibt_praid_is_sync_cmd_run_dirty_rebuild(sync_cmd) || nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(sync_cmd)) {
			N_Tf(vt65431, "Do not revive the seg=@UUID_8, we are in the middle of recovery process", SEG_UUID_8);
    		goto out;
		}

		N_Tf(oi98mch, "seg=@UUID_8 is back from dead to alive", SEG_UUID_8);
		// Dead is always considered out of sync
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(bgy76tf, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		// The remote might think that it was stable. Correcting. Used to identify stable shutdown for init_modes.
		NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(xcbo98e, &disk_segment->seg_leader, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		goto consider_accepting_remote_INIT_MODE;
    }
	// If back to life after
	// 1. A stable (relevant praid_version wise) shutdown
	// 2. An unstable shutdown
	if (nvmeibt_disk_segment_is_alive_stable(remote_seg_topo) || nvmeibt_disk_segment_is_alive_unstable(remote_seg_topo)) {
		N_Tf(sjenc4d, "seg=@UUID_8 is back from dead to alive. Careful not to lose ownership etc.", SEG_UUID_8);
		if (nvmeibt_seg_lot_is_owner_of_any_seg_in_baseline_topo(calculated_seg_lot)) {
			// A quick down & up of the remote TOMA during work, we didn't even set it to DEAD
			// Was an owner, and will remain an owner
			N_Tf(tvsghwl, "seg=@UUID_8 remotely @STR. Do not lose ownership, INIT required", SEG_UUID_8, dirty_bits_state_str(remote_seg_topo->dirty_bits_state));
			// ALIVE_STABLE:
			// - Saved all the dirty+stale after stopping all I/O (implying that the pRAID was frozen I/O wise)
			// - The remote_applied_topo_ctx.dirty_bits_state indicated that it is ALIVE_STABLE. I.e., lost its memory and required init FROM_PERSIST
			// ALIVE_UNSTABLE:
			// - Nothing to say
			// In any case:
			// - We keep the ownership, but require init
			// - If we will decide to do a cold-recovery, then the ownership will hold
			// - If we will decide to continue hot, then we will put it under_recovery
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(6bhdu92, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
		} else {
			// Its memory is irrelevant (no dirty for non-owner)
			// We just need to restart the dirty-rebuild from scratch. Need to init.
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(f5iswma, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE);
		}
		if (calculated_seg_topo->dirty_bits_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER)
			nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED);
		goto consider_accepting_remote_INIT_MODE;
	}
	// If OWNER_RECOVERER_DONE with the "current" praid_version then update it
	if (	(calculated_seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER) &&
			(remote_seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE)) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(u7u8u6t, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE);
		goto consider_accepting_remote_INIT_MODE;
	}
	// If EC_COLD_RECOVERER_DONE with the "current" praid_version then update it
	if (	(calculated_seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER) &&
			(remote_seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE)) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(lo09knv, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE);
		goto consider_accepting_remote_INIT_MODE;
	}
	// If UNDER_RECOVERY_I is INIT_DONE then switch to UNDER_RECOVERY_R
	if (	(calculated_seg_topo->dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I) &&
			nvmeibt_disk_segment_is_mem_tbl_init_done_fully(remote_seg_topo)) {
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(jdhfus7, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R);
		goto consider_accepting_remote_INIT_MODE;
	}

	// In all other cases, the current topology rules.
	// Especially, keep the precious owners group
	// transitions from OWNER_* had to first go through DEAD
consider_accepting_remote_INIT_MODE:
	if (is_remote_active_fully_synched && nvmeibt_praid_is_client_sync_cmd_initializing(sync_cmd)) {
		// Update from peer's dirty_bits/stale_locks _init_mode,
		//  actually, for the case of NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE
		if (remote_seg_topo->dirty_bits_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE)
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE(5vpwmhx, calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
		if (remote_seg_topo->stale_locks_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE)
			NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(cvsikw0, calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE);
	}
out:
	NFOUT;
}

/**
 * I am the leader.
 * Only reading of the state. Not calculating a new topology.
 * Note that this function is not called if the report is
 * identical to the peer's previous report. (still, the change
 * might be irrelevant)
 */
enum nvmeibt_add_rv nvmeibt_disk_segment_leader_upd_from_peer_applied(struct nvmeibt_serialized_seg_active_topo *seg_active_topo,
																	  unsigned long long local_serialization_version,
																	  struct nvmeibt_raft_member *remote_member)
{
	enum nvmeibt_add_rv						rv = NVMEIBT_ADD_FAILED;
	struct nvmeibt_disk_segment				*disk_segment;
	struct nvmeibt_praid					*praid;
	struct nvmeibt_disk_segment_topo_ctx	*peer_topo;
	struct nvmeibt_disk_segment_topo_ctx	input_peer_topo;
	struct nvmeibt_disk						*its_disk;
	int										SEG_UUID_8;

	TODO(dumper);

	input_peer_topo.active_seg_flags = seg_active_topo->active_seg_flags;
	disk_segment = nvmeibt_disk_segment_get_disk_segment_by_id(&seg_active_topo->uuid);
	if (!disk_segment) {
		N_Tf(hyhdcb4, "Segment not found. Ignoring!");
		rv = NVMEIBT_ADD_SKIPPED;
		goto out;
	}
	SEG_UUID_8 = nvmeibt_seg_UUID_8(disk_segment);
	disk_segment->seg_leader.last_remote_applied_node_local_serialization_version = local_serialization_version;
	peer_topo = &disk_segment->seg_leader.remote_seg_topo;
	praid = disk_segment->seg_mgmt.its_praid;
	N_Tf(p09oki8, "Received: seg=@UUID_8 peer_applied_dirty_bits_state=@DIRTY_BITS_STATE(old=@DIRTY_BITS_STATE) peer_applied_praid_version=@PRAID_VERSION:@PRAID_VERSION node_uuid=@UUID_LE flg=@X",
		SEG_UUID_8,
		dirty_bits_state_str(seg_active_topo->dirty_bits_state), dirty_bits_state_str(peer_topo->dirty_bits_state),
		seg_active_topo->active_praid_version_major, seg_active_topo->active_praid_version_minor, nvmeibt_raft_member_id(remote_member), seg_active_topo->active_seg_flags_int);
	if (!praid) {
		N_Tf(hy65472, "No PRAID");
		goto out;
	}
	// If the disk moved to a different node, then update it
	its_disk = disk_segment->seg_mgmt.its_disk;
	if (its_disk) {
		its_disk->is_drive_write_error |= input_peer_topo.active_seg_flags.is_drive_write_error;
		disk_segment->is_drive_write_error |= input_peer_topo.active_seg_flags.is_drive_write_error;
		if ((!(its_disk->leader_its_raft_member) || (remote_member != its_disk->leader_its_raft_member))) {
			nvmeibt_topology_leader_connect_disk_with_raft_member(its_disk, remote_member);
			NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(mj8891x, praid);
		}
	}
	// If there were changes then we might need to regenerate the PRAID's topology
	if (	peer_topo->active_seg_ser_ver != seg_active_topo->active_seg_ser_ver ||
			(peer_topo->seg_praid_version_major != seg_active_topo->active_praid_version_major) ||
			(peer_topo->seg_praid_version_minor != seg_active_topo->active_praid_version_minor) ) {
		N_Tf(fty7659, "remote_applied... are_praid_registrants_aligned:@ARE_PRAID_REGISTRANTS_ALIGNED_WITH_SYNC_CMD->@ARE_PRAID_REGISTRANTS_ALIGNED_WITH_SYNC_CMD dirty_bits_state:@DIRTY_BITS_STATE_STR->@DIRTY_BITS_STATE_STR "
				"dirty_bits_init:@MEM_CTL_INIT_MODE_STR->@MEM_CTL_INIT_MODE_STR stale_locks_init:@MEM_CTL_INIT_MODE_STR->@MEM_CTL_INIT_MODE_STR ser_ver:@ACTIVE_SEG_SER_VER->@ACTIVE_SEG_SER_VER praid_ver:@PRAID_VERSION.@PRAID_VERSION->@PRAID_VERSION.@PRAID_VERSION",
				peer_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd, input_peer_topo.active_seg_flags.are_praid_registrants_aligned_with_sync_cmd,
				dirty_bits_state_str(peer_topo->dirty_bits_state), dirty_bits_state_str(seg_active_topo->dirty_bits_state),
				mem_tbl_init_mode_str(peer_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(seg_active_topo->dirty_bits_init_mode),
				mem_tbl_init_mode_str(peer_topo->stale_locks_init_mode), mem_tbl_init_mode_str(seg_active_topo->stale_locks_init_mode),
				peer_topo->active_seg_ser_ver, seg_active_topo->active_seg_ser_ver,
				peer_topo->seg_praid_version_major, peer_topo->seg_praid_version_minor,
				seg_active_topo->active_praid_version_major, seg_active_topo->active_praid_version_minor);
		praid->praid_leader.did_any_client_report_about_problems |= input_peer_topo.active_seg_flags.did_any_client_report_about_problems;
		NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(ooo99d2, praid);
		peer_topo->active_seg_ser_ver = seg_active_topo->active_seg_ser_ver;
		NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS(sdy765c, &disk_segment->seg_leader, seg_active_topo->dirty_bits_state);
		peer_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = input_peer_topo.active_seg_flags.are_praid_registrants_aligned_with_sync_cmd;
		NNVMEIBT_SEG_REMOTE_SET_DIRTY_BITS_INIT_MODE(ju9wsn6, &disk_segment->seg_leader, seg_active_topo->dirty_bits_init_mode);
		NNVMEIBT_SEG_REMOTE_SET_STALE_LOCKS_INIT_MODE(yzvbjrw, &disk_segment->seg_leader, seg_active_topo->dirty_bits_init_mode);	TODO(A hack. Otherwise, TXID looks as if not INIT_DONE...);
		NNVMEIBT_SEG_REMOTE_SET_STALE_LOCKS_INIT_MODE(xdsfp05, &disk_segment->seg_leader, seg_active_topo->stale_locks_init_mode);
	} else {
		N_Tf(ftyraq2, "No need to do anything");
	}
	peer_topo->seg_praid_version_major = seg_active_topo->active_praid_version_major;
	peer_topo->seg_praid_version_minor = seg_active_topo->active_praid_version_minor;
	rv = NVMEIBT_ADD_NEW;
out:
	return rv;
}

// Upd committed topo on startup (from persist) or when received from the leader
enum nvmeibt_add_rv nvmeibt_seg_follower_upd_committed_seg_topo(struct nvmeibt_serialized_seg_leader_topo *in_leader_serialized_seg_topo)
{
	enum nvmeibt_add_rv						rv;
	union nvmeib_uuid						*topo_segment_id;
	struct nvmeibt_disk_segment				*disk_segment;
//	struct nvmeibt_disk						*disk = NULL;
	struct nvmeibt_praid					*praid = NULL;
	struct nvmeibt_disk_segment_topo_ctx	*committed_seg_topo = NULL;
	struct nvmeibt_disk_segment_topo_ctx	in_seg_topo = {0};
	struct nvmeibt_disk_segment_topo_ctx	prev_seg_topo;
	int										SEG_UUID_8;
	struct nvmeibt_seg_lot					*committed_seg_lot;
	struct nvmeibt_praid_lot				*committed_praid_lot;
	int8_t									topo_owner_idx, topo_secondary_owner_idx;

	in_seg_topo.dirty_bits_state = in_leader_serialized_seg_topo->dirty_bits_state;
	in_seg_topo.seg_praid_version_major = in_leader_serialized_seg_topo->praid_version_major;
	in_seg_topo.seg_praid_version_minor = in_leader_serialized_seg_topo->praid_version_minor;
	in_seg_topo.dirty_bits_init_mode = in_leader_serialized_seg_topo->dirty_bits_init_mode;
	in_seg_topo.stale_locks_init_mode = in_leader_serialized_seg_topo->stale_locks_init_mode;
	in_seg_topo.txid_init_mode = in_leader_serialized_seg_topo->txid_init_mode;
	in_seg_topo.is_registrants_synchronizer = in_leader_serialized_seg_topo->is_registrants_synchronizer;
	in_seg_topo.leader_seg_flags = in_leader_serialized_seg_topo->leader_seg_flags;

	topo_segment_id = &in_leader_serialized_seg_topo->uuid;
	topo_owner_idx = in_leader_serialized_seg_topo->owner_idx;
	topo_secondary_owner_idx = in_leader_serialized_seg_topo->secondary_owner_idx;

	disk_segment = nvmeibt_disk_segment_get_disk_segment_by_id(topo_segment_id);
	if (!disk_segment) {
		if (nvmeibt_disk_segment_is_x(&in_seg_topo)) {
			N_Tf(ji8u765, "X seg=@UUID_8 was removed from config. Ignoring.", nvmeib_uuid_first_4_bytes(topo_segment_id));
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
		}
		else {
			if (topo_owner_idx == DUMMY_OWNER) {
				N_Wf(u78u4c3, "seg=@UUID_8 dirty_bits=@DIRTY_BITS_STATE_STR inits=(d=@DIRTY_BITS_INIT_MODE s=@STALE_LOCKS_INIT_MODE t=@TXID_INIT_MODE) does not exist in config, probably obsolete raft_only seg",
					 nvmeib_uuid_first_4_bytes(topo_segment_id), dirty_bits_state_str(in_seg_topo.dirty_bits_state),
					 mem_tbl_init_mode_str(in_seg_topo.dirty_bits_init_mode), mem_tbl_init_mode_str(in_seg_topo.stale_locks_init_mode), mem_tbl_init_mode_str(in_seg_topo.txid_init_mode));
				rv = NVMEIBT_ADD_SKIPPED;
			} else {
				N_Wf(xft674b, "Surprise delete seg=@UUID_8. Existed in the old topo (not X), but does not exist in config", nvmeib_uuid_first_4_bytes(topo_segment_id));
				rv = NVMEIBT_ADD_MODIFIED; // NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL
			}
		}
		goto out;
	}
	SEG_UUID_8 = nvmeibt_seg_UUID_8(disk_segment);

	praid = disk_segment->seg_mgmt.its_praid;
	if (!praid) {
		N_Ef(irir843, "No praid seg=@UUID_8", SEG_UUID_8);
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}
	committed_seg_lot = &disk_segment->seg_follower.committed_seg_lot;

	committed_praid_lot = &praid->praid_follower.committed_praid_lot;
	in_seg_topo.owner_seg_lot = (topo_owner_idx == DUMMY_OWNER) ? NULL : committed_praid_lot->topo_seg_lots[topo_owner_idx];
	in_seg_topo.secondary_owner_seg_lot = (topo_secondary_owner_idx == DUMMY_OWNER) ? NULL : committed_praid_lot->topo_seg_lots[topo_secondary_owner_idx];

	rv = NVMEIBT_ADD_MODIFIED;
	committed_seg_topo = &committed_seg_lot->seg_topo;
	prev_seg_topo = *committed_seg_topo;
	// We have a new topo, accept it as committed - no questions asked
	*committed_seg_topo = in_seg_topo;
	praid->was_praid_ever_activated |= nvmeibt_disk_segment_is_dirty_bits_state_calculated(committed_seg_topo->dirty_bits_state);

	nvmeibt_disk_segment_dump(disk_segment);
	N_Tf(u87unbt, "seg=@UUID_8 committed_topo_dirty_bits_state=@DIRTY_BITS_STATE_STR(cur=@DIRTY_BITS_STATE_STR) "
		"praid_version=@PRAID_VERSION:@PRAID_VERSION owner_seg=@UUID_8",
		SEG_UUID_8, dirty_bits_state_str(committed_seg_topo->dirty_bits_state), dirty_bits_state_str(prev_seg_topo.dirty_bits_state),
		committed_seg_topo->seg_praid_version_major, committed_seg_topo->seg_praid_version_minor, nvmeibt_seg_lot_UUID_8(committed_seg_topo->owner_seg_lot)) ;
	N_Tf(srtm98x, "dirty_bits_init_mode=@DIRTY_BITS_INIT_MODE stale_locks_init_mode=@STALE_LOCKS_INIT_MODE txid_init_mode=@TXID_INIT_MODE is_synchronizer=@IS_SYNCHRONIZER",
		mem_tbl_init_mode_str(committed_seg_topo->dirty_bits_init_mode), mem_tbl_init_mode_str(committed_seg_topo->stale_locks_init_mode),
		mem_tbl_init_mode_str(committed_seg_topo->txid_init_mode), committed_seg_topo->is_registrants_synchronizer);

out:
	if (disk_segment && (rv == NVMEIBT_ADD_MODIFIED)) {
		if (committed_seg_topo && (memcmp(&prev_seg_topo, committed_seg_topo, sizeof(*committed_seg_topo)) == 0)) {
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
		}
	}
	return rv;
}

static void remove_seg_from_all_lists(struct nvmeibt_disk_segment *seg)
{
	XDLIST_DEL(&seg->praid_all_segs_link);
	XDLIST_DEL(&seg->seg_leader.baseline_seg_lot.praid_all_seg_lots_link);
	XDLIST_DEL(&seg->seg_leader.calculated_seg_lot.praid_all_seg_lots_link);
	XDLIST_DEL(&seg->seg_leader.to_report_seg_lot.praid_all_seg_lots_link);
	XDLIST_DEL(&seg->seg_follower.committed_seg_lot.praid_all_seg_lots_link);
	XDLIST_DEL(&seg->seg_follower.applied_seg_lot.praid_all_seg_lots_link);
}

/*
 * Try to do remove a disk segment. Returns:
 *   1  : entry removed
 *   0  : no entries removed
 */
enum nvmeibt_seg_remove_rv nvmeibt_disk_segment_remove(struct nvmeibt_disk_segment *disk_segment)
{
	int							j;
	struct nvmeibt_praid		*praid;
	struct nvmeibt_disk			*disk;
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_seg_active	*seg_active;
	int8_t						idx_in_topo_segs;
	enum nvmeibt_seg_remove_rv	rv = NVMEIBT_SEG_STILL_IN_CONFIG;

	NFIN;

	seg_active = nvmeibt_disk_segment_get_seg_active(disk_segment);
	if (nvmeibt_seg_active_is_zeroing_state_WQ_task_in_work(seg_active)) {
		seg_active->applied_zeroing_state = NVMEIBT_ZEROING_STATE_NOT_NEEDED;
		N_Tf(ty786me, "Not yet. seg=@UUID_8 is during zeroing", nvmeibt_seg_UUID_8(disk_segment));
    	rv = NVMEIBT_SEG_ZEROING_IN_PROCESS;
    	goto out;
	}

	nvmeibt_seg_active_stop_recovery_tasks(seg_active);
	if (nvmeibt_seg_active_is_any_recovery_in_the_air(seg_active)) {
		N_Tf(sc64b9c, "Not yet. seg=@UUID_8 still has recovery_in_the_air", nvmeibt_seg_UUID_8(disk_segment));
		rv = NVMEIBT_SEG_RECOVERY_IN_PROCESS;
		goto out;
	}

	// nvmeibt_seg_active_mark_zeroing_required_as_needed(seg_active);
	if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(disk_segment)) {
		N_Ef(as4561c, "Not yet. seg=@UUID_8 state=@STATE_STR is still in config",
			 nvmeibt_seg_UUID_8(disk_segment), dirty_bits_state_str(disk_segment->seg_follower.applied_seg_lot.seg_topo.dirty_bits_state));
		goto out;
	}
	praid = disk_segment->seg_mgmt.its_praid;
	disk = disk_segment->seg_mgmt.its_disk;
	N_Tf(de4320m, "Removing seg=@UUID_8", nvmeibt_seg_UUID_8(disk_segment));
	// Remove from its disk
	if (disk) {
		for (j = disk->n_segments - 1; j >= 0; --j) {
			if (disk->disk_segments[j] == disk_segment) {
				disk->disk_segments[j] = disk->disk_segments[--(disk->n_segments)];
				disk->disk_segments[(disk->n_segments)] = NULL;
				break;
			}
		}
	}

	local_disk = nvmeibt_seg_active_get_local_disk(seg_active);
	if (!nvmeibt_local_disk_is_being_deleted(local_disk)) {
		nvmeibt_disk_metadata_remove_entry_from_mem_gpt(&local_disk->main_gpt, nvmeibt_seg_UUID(disk_segment));
		nvmeibt_disk_metadata_remove_entry_from_mem_gpt(&local_disk->metadata_gpt, nvmeibt_seg_UUID(disk_segment));

		// Update the persistency struct to reflect the change
		NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(vgsh8k3, local_disk);
	}

	// Remove from cur_topo
	nvmeib_hash_delete_uuid(nvmeibt_global_get_global()->disk_segments_hash_by_uuid, &(disk_segment->from_config.id));

	if (!praid)
		goto out_and_remove;

	// Remove it from its praid, be it under segment or replacement_segments
	remove_seg_from_all_lists(disk_segment);
	idx_in_topo_segs = nvmeibt_disk_segment_idx_in_praid(disk_segment);
	if (praid->praid_mgmt.topo_segs[idx_in_topo_segs] == disk_segment) {
		praid->praid_mgmt.topo_segs[idx_in_topo_segs] = praid->praid_mgmt.replacement_topo_segs[idx_in_topo_segs];
	}
	if (praid->praid_mgmt.replacement_topo_segs[idx_in_topo_segs] == disk_segment) {
		praid->praid_mgmt.replacement_topo_segs[idx_in_topo_segs] = NULL;
	}

out_and_remove:
	NVMEIBT_SEG_ACTIVE_FREE_MEM_AND_PROCESSES(seg_active);
	NNVMEIBT_BM_FREE(ki98761, disk_segment);
	rv = NVMEIBT_SEG_REMOVED;

out:
	NFOUT;
	return rv;
}

void nvmeibt_disk_segment_garbage_collect_old_segments(bool *is_any_garbage_collected, bool *is_all_garbage_collected)
{
	struct nvmeibt_disk_segment		*seg;

	NFIN;
    *is_any_garbage_collected = 0;
    *is_all_garbage_collected = 1;
	// Look for old segs, including orphans (not connected to praid, not even as replacement_topo_segs). Scan all segs.
	NVMEIB_HASH_FOREACH(seg, nvmeibt_global_get_global()->disk_segments_hash_by_uuid) {
		if (!NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(seg)) {
			continue;   // Only segments that were removed from config can be removed. Otherwise, we still need to report them to mgmt
		}
		if (nvmeibt_disk_segment_remove(seg) == NVMEIBT_SEG_REMOVED) {
			*is_any_garbage_collected = 1;
		} else {
			*is_all_garbage_collected = 0;
		}
	}
	NFOUT;
}

void nvmeibt_disk_segment_mark_conf_corrupted(struct nvmeibt_disk_segment *seg)
{
	N_Tf(utim782, "seg=@UUID_8", nvmeibt_seg_UUID_8(seg));
	seg->from_config.version = 0xDEADBEEF;
	seg->is_conf_corrupted = 1;
	nvmeibt_mark_conf_corrupted();
}

void nvmeibt_seg_lot_mark_conf_corrupted(struct nvmeibt_seg_lot *seg_lot)
{
	nvmeibt_disk_segment_mark_conf_corrupted(seg_lot->my_seg);
}

void nvmeibt_disk_segment_trim_specific_seg(struct nvmeibt_disk_segment *seg, uint8_t trim_flag)
{
	if (is_trim_needed(&seg->trim_flags, trim_flag))
		NVMEIBT_HASH_MARK_OBJ_OUTDATED(dju87e9, seg, seg);
	else
		N_Tf(nsse442, "not yet, seg=@UUID_8 flags=@X", nvmeibt_seg_UUID_8(seg), seg->trim_flags);
		// A disk_segment that was removed from mgmt_config, was previously
		// 1. Reported deprecated by TOMA
		// 2. Before this it was zeroed as needed
		// 3. Before this all activity was stopped
		// Now a new seg might reside on the same blocks, so before activating the new
		//  seg, we have to make certain that nothing will happen on the old seg's blocks.
		// The solution is to update the zeroing progress, so once done, we do not rerun
		//  it also after boot.
}

void nvmeibt_disk_segment_trim_unused_entries(int config_tag, uint8_t trim_flag)
{
	struct nvmeibt_disk_segment		*disk_segment;

	NFIN;
	NVMEIB_HASH_FOREACH(disk_segment, nvmeibt_global_get_global()->disk_segments_hash_by_uuid) {
		if (NVMEIBT_HASH_IS_OLDER_OBJ(disk_segment, config_tag)) {
			nvmeibt_disk_segment_trim_specific_seg(disk_segment, trim_flag);
		}
	}

	// The disk_segments will be removed as part of the volume's garbage collection

	NFOUT;
}

bool nvmeibt_seg_lot_is_owner_of_any_seg_in_baseline_topo(struct nvmeibt_seg_lot *seg_lot)
{
	int											is_owner = 0;
	int											i;
	struct nvmeibt_praid						*praid = seg_lot->my_seg->seg_mgmt.its_praid;
	struct nvmeibt_praid_lot					*praid_lot = &praid->praid_leader.baseline_praid_lot;
	struct nvmeibt_seg_lot						*other_seg_lot;
	struct nvmeibt_seg_lot						*baseline_seg_lot = seg_lot_get_baseline_seg_lot(seg_lot);
	struct nvmeibt_disk_segment_topo_ctx		*baseline_seg_topo;

//	FIN;
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		other_seg_lot = praid_lot->topo_seg_lots[i];
		if (other_seg_lot->seg_topo.owner_seg_lot == baseline_seg_lot) {
			baseline_seg_topo = &baseline_seg_lot->seg_topo;
			is_owner = 1;
			if (!nvmeibt_disk_segment_is_competent_owner(baseline_seg_topo) && !(nvmeibt_praid_is_deprecated_in_config(praid))) {
				N_Wf(fgy7320, "OOPS! seg=@UUID_8 other_seg=@UUID_8 @DIRTY_BITS_STATE_STR", nvmeibt_seg_lot_UUID_8(baseline_seg_lot), nvmeibt_seg_lot_UUID_8(other_seg_lot),
					dirty_bits_state_str(baseline_seg_topo->dirty_bits_state));
				is_owner = 0;
			}
			break;
		}
	}
//	FOUT;
	return is_owner;
}

void nvmeibt_disk_segment_active_mark_reserialization_required(struct nvmeibt_disk_segment *seg)
{
	N_Tf(dt67jex, "seg=@UUID_8", nvmeibt_seg_UUID_8(seg));
	nvmeibt_topology_active_mark_reserialization_required();
}

bool nvmeibt_disk_segment_are_topos_actionably_different(const union nvmeib_uuid *uuid, struct nvmeibt_disk_segment_topo_ctx *new_t, struct nvmeibt_disk_segment_topo_ctx *old_t)
{
	bool is_diff =
		(	(new_t->dirty_bits_state !=			old_t->dirty_bits_state &&
			 !nvmeibt_disk_segment_is_owner_recoverer_done(new_t) && !nvmeibt_disk_segment_is_ec_cold_recoverer_done(new_t)) ||
		 !ARE_UUID_EQ(nvmeibt_seg_lot_UUID(new_t->owner_seg_lot), nvmeibt_seg_lot_UUID(old_t->owner_seg_lot)) ||
		 !ARE_UUID_EQ(nvmeibt_seg_lot_UUID(new_t->secondary_owner_seg_lot), nvmeibt_seg_lot_UUID(old_t->secondary_owner_seg_lot)) ||
		 (new_t->dirty_bits_init_mode != old_t->dirty_bits_init_mode 	&& new_t->dirty_bits_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE) ||
		 (new_t->stale_locks_init_mode != old_t->stale_locks_init_mode	&& new_t->stale_locks_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE) ||
		 (new_t->txid_init_mode != old_t->txid_init_mode				&& new_t->txid_init_mode != NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE));
	if (is_diff) {
		N_Tf(gevs7hi, "seg=@UUID_8 dirty_bits_state=@STR(@STR) owner_seg=@UUID_8(@UUID_8)",
			nvmeib_uuid_first_4_bytes(uuid), dirty_bits_state_str(new_t->dirty_bits_state), dirty_bits_state_str(old_t->dirty_bits_state),
			nvmeibt_seg_lot_UUID_8(new_t->owner_seg_lot), nvmeibt_seg_lot_UUID_8(old_t->owner_seg_lot));
		N_Tf(3mslxwp, "dirty_bits_init_mode=@STR(@STR) stale_locks_init_mode=@STR(@STR) txid_init_mode=@STR(@STR)",
			mem_tbl_init_mode_str(new_t->dirty_bits_init_mode), mem_tbl_init_mode_str(old_t->dirty_bits_init_mode),
			mem_tbl_init_mode_str(new_t->stale_locks_init_mode), mem_tbl_init_mode_str(old_t->stale_locks_init_mode),
			mem_tbl_init_mode_str(new_t->txid_init_mode), mem_tbl_init_mode_str(old_t->txid_init_mode));
	}
	return is_diff;
}

