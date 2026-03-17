/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_common.h"
#include "nvmeibt_important_logs.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topo_bin.h"
#include "nvmeibt_disk_segment.h"
#include <inttypes.h> // For PRIx64

int64_t praid_time_from_activation_attempt_to_degraded_mode_sec = PRAID_ACTIVATION_TIMEOUT_SEC_DEFAULT;

extern const union nvmeib_uuid nvmeibt_dummy_uuid;

struct dirty_bits_state_stats {
	int							alive_stable;
	int							alive_unstable;
	int							dead;
	int							x;
	int							under_recovery_I;			// regular _I or NEW_I
	int							under_recovery_R;			// regular _R or NEW_R
	int							launched_recoverers;		// COLD / HOT (only one of them will be active at a time)
	int							owners_recoverer;
	int							owners_recoverer_done;
	int							owners_idle;
	int							owners_recovered;
	int							owners_deprecating;
	int							remote_applied_de_facto_owners;
	int							alive;
	int							should_be_alive;
	int							competent_owners;
	int							de_facto_owners;
	int							any_owner;
	int							non_crashed_de_facto_owners;
	int							recoverable_stable_de_facto_owners;
	int							new_under_recov_I_or_R;
	int							new_dead;
	int							owner_died;
	int							non_owner_died;
	int							cold_recoverer;
	int							cold_recoverer_done;
	int							init_due_now;
	BOOL						are_all_owners_alive;
	BOOL						are_all_segments_alive;
	BOOL						is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty;
	BOOL						is_degraded;
	BOOL						is_it_possible_that_dirty_bits_were_set;
	BOOL						is_cold_recovery_init_needed;
	BOOL						is_cold_recovery;
	BOOL						is_hot_recovery;
};

void nvmeibt_praid_inc_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid)
{
	(praid->n_recoveries_needing_hidden_attach)++;
	N_Tf(6bhe8dk, "praid=@UUID_LE n=@INT", nvmeibt_praid_UUID(praid), praid->n_recoveries_needing_hidden_attach);
}
void nvmeibt_praid_dec_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid)
{
	--(praid->n_recoveries_needing_hidden_attach);
	N_Tf(v46skoe, "praid=@UUID_LE n=@INT", nvmeibt_praid_UUID(praid), praid->n_recoveries_needing_hidden_attach);
}
int nvmeibt_praid_get_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid)
{
	return (praid ? praid->n_recoveries_needing_hidden_attach : 0);
}

BOOL nvmeibt_praid_applied_is_qualify_for_sync_stale(struct nvmeibt_praid *praid)
{
	int										i;
	BOOL									is_qualify;
	struct nvmeibt_praid_lot				*applied_praid_lot;
	struct nvmeibt_seg_lot					*applied_seg_lot;

	if (!praid) {
		is_qualify = false;
		goto out;
	}
	applied_praid_lot = &praid->praid_follower.applied_praid_lot;

	if (nvmeibt_praid_is_jbod(praid)) {
		is_qualify = false;
	} else if (nvmeibt_praid_is_type_RAID1(praid)) {
		// For PRAID we only run stale rebuild after back to normal. Avoid converting stale to dirty
		is_qualify = true;
		for (i = 0; i < applied_praid_lot->n_topo_seg_lots; i++) {
			applied_seg_lot = applied_praid_lot->topo_seg_lots[i];
			is_qualify &= (applied_seg_lot && applied_seg_lot->seg_topo.dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
		}
	}
	else { // not jbod and not raid1 - so EC
		// Since journal entries are a scarce resource, we recover them also at the cost of converting to dirty.
		is_qualify = true;
	}

out:
	return is_qualify;
}

BOOL nvmeibt_praid_applied_is_qualify_for_JGC(struct nvmeibt_praid *praid)
{
	BOOL									is_qualify = false;
	struct nvmeibt_praid_topo_ctx			*applied_praid_topo;

	if (!praid)
		goto out;

	applied_praid_topo = &praid->praid_follower.applied_praid_lot.topo_ctx;
	if (	!applied_praid_topo->is_activated ||
			nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(applied_praid_topo->registrants_sync_cmd)) {
		goto out;
	}
	is_qualify = true;

out:
	return is_qualify;
}

bool nvmeibt_praid_is_deprecated_in_config(const struct nvmeibt_praid *praid)
{
	return (praid && nvmeibt_chunk_is_deprecated_in_config(praid->praid_mgmt.its_chunk));
}

struct nvmeibt_block_device *nvmeibt_praid_get_blkdev(struct nvmeibt_praid *praid)
{
	return ((praid && praid->praid_mgmt.its_chunk) ? praid->praid_mgmt.its_chunk->its_block_device : NULL);
}

const char *nvmeibt_praid_get_type_str(const struct nvmeibt_praid *praid)
{
	return nvmeibt_praid_type_str(praid->praid_mgmt.type);
}

/***************        *****************/

enum NVMEIBT_CLIENT_TR_REASON nvmeibt_praid_applied_sync_cmd_reason(struct nvmeibt_praid *praid)
{
	switch (praid->praid_follower.applied_praid_lot.topo_ctx.registrants_sync_cmd) {
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:			return NVMEIBT_CLIENT_TR_REASON_INIT;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:			return NVMEIBT_CLIENT_TR_REASON_SW_TOPO_W;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:			return NVMEIBT_CLIENT_TR_REASON_SW_TOPO_D;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:	return NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_UNSAFE;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:	return NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_SAFE;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:			return NVMEIBT_CLIENT_TR_REASON_SW_TOPO_X;
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:		return NVMEIBT_CLIENT_TR_REASON_AWAITING_CLIENTS_SYNC;
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:					return NVMEIBT_CLIENT_TR_REASON_DELETING_SEG;
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I:		return NVMEIBT_CLIENT_TR_REASON_INIT;
	default:												return NVMEIBT_CLIENT_TR_REASON_TBD;
	}
}

struct nvmeibt_praid *nvmeibt_praid_get_praid_by_id(const union nvmeib_uuid *id)
{
	struct nvmeibt_praid 	*praid;

	praid = nvmeib_hash_search_uuid(nvmeibt_global_get_global()->praids_hash_by_uuid, id);

	if (praid == NULL) {
		N_Tf(gty765r, "praid not found id=@UUID_LE", id);
	}

	return praid;
}

const char *nvmeibt_praid_get_blkdev_name(const struct nvmeibt_praid *praid)
{
	return (praid ? nvmeibt_chunk_get_blkdev_name(praid->praid_mgmt.its_chunk) : "???");
}

static void leader_switch_to_replacement_seg(struct nvmeibt_praid_lot *calculated_praid_lot, int8_t idx_in_topo_segs)
{
	struct nvmeibt_seg_lot					*seg_lot;
	struct nvmeibt_seg_lot					*replacement_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*replacement_seg_topo;

	NFIN;

	seg_lot = calculated_praid_lot->topo_seg_lots[idx_in_topo_segs];
	replacement_seg_lot = calculated_praid_lot->replacement_topo_seg_lots[idx_in_topo_segs];
	if (replacement_seg_lot) {
		seg_topo = &seg_lot->seg_topo;
		if (!nvmeibt_disk_segment_is_x_done(seg_topo)) {
			N_Wf(b4bhswm, "Replaced seg=@UUID_8 @DIRTY_BITS_STATE not X_DONE", nvmeibt_seg_lot_UUID_8(seg_lot), dirty_bits_state_str(seg_lot->seg_topo.dirty_bits_state));
		}
		N_Tf(pqbaknd, "Replacing seg=@UUID_8 with rep_seg=@UUID_8", nvmeibt_seg_lot_UUID_8(seg_lot), nvmeibt_seg_lot_UUID_8(replacement_seg_lot));
		// If not already replaced, then do the replacement&init (only once)
		replacement_seg_topo = &replacement_seg_lot->seg_topo;
		// nvmeibt_seg_lot_reset_topo_ctx(replacement_seg_lot);
		replacement_seg_topo->owner_seg_lot = seg_topo->owner_seg_lot;
		replacement_seg_topo->secondary_owner_seg_lot = NULL; //seg_topo->secondary_owner_seg_lot;

		replacement_seg_lot->is_replacement = 0;
		calculated_praid_lot->topo_seg_lots[idx_in_topo_segs] = replacement_seg_lot;
		calculated_praid_lot->replacement_topo_seg_lots[idx_in_topo_segs] = NULL;
		XDLIST_DEL(&seg_lot->praid_all_seg_lots_link);
		nvmeibt_disk_segment_trim_specific_seg(seg_lot->my_seg, CONFIG_TRIM_MGMT);
	}
	else {
		N_Ef(u87sdg5, "Replacement seg for seg=@UUID_8 does non exist", nvmeibt_seg_lot_UUID_8(seg_lot));
	}

	NFOUT;
}

void nvmeibt_praid_mark_all_praid_segs_post_update_actions_required(struct nvmeibt_praid *praid)
{
	int								i;
	struct nvmeibt_seg_active		*seg_active;

	NFIN;
	for (i = 0; i < praid->praid_follower.applied_praid_lot.n_topo_seg_lots; i++) {
		seg_active = nvmeibt_disk_segment_get_seg_active(praid->praid_follower.applied_praid_lot.topo_seg_lots[i]->my_seg);
		NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(cnndd82, seg_active);
	}
	NFOUT;
}

int nvmeibt_praid_get_blkdev_config_version(struct nvmeibt_praid *praid)
{
	return (praid && praid->praid_mgmt.its_chunk && praid->praid_mgmt.its_chunk->its_block_device ?
			praid->praid_mgmt.its_chunk->its_block_device->from_config.version : -1);
}

void nvmeibt_praid_mark_conf_corrupted(struct nvmeibt_praid *praid)
{
	N_Wf(rty7643, "praid=@UUID_LE conf_corrupted", nvmeibt_praid_UUID(praid));
	if (praid) {
		if (!praid->praid_mgmt.is_conf_corrupted) {
			praid->praid_mgmt.is_conf_corrupted = 1;
			NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(ol19928, praid);
		}
		nvmeibt_mark_conf_corrupted();
	}
}

static int squash_praid_segs(struct nvmeibt_disk_segment **segments, int8_t n_segs)
{
	struct nvmeibt_disk_segment 		**first_seg;
	struct nvmeibt_disk_segment 		**last_seg;

	if (n_segs == 0)
		return 0;

	first_seg = &segments[0];
	last_seg = &segments[n_segs - 1];

	while (first_seg < last_seg) {
		if (*first_seg) {
			first_seg++;
			continue;
		}
		assert(*last_seg);
		*first_seg = *last_seg;
		last_seg--;
		while (!*last_seg) {
			if (last_seg > first_seg)
				last_seg--;
			else
				goto out;
		}
	}

out:
	return (first_seg - segments) + 1;
}

static int validate_praid_seg(struct nvmeibt_praid *praid, struct nvmeibt_disk_segment *disk_segment)
{
	int		rv = 0;

	if (nvmeibt_disk_segment_get_praid(disk_segment) != praid) {
		rv = -1;
		N_Ef(rty7648, "praid=@UUID_LE, seg=@UUID_8, its_praid=@UUID_LE",
			 nvmeibt_praid_UUID(praid), nvmeibt_seg_UUID_8(disk_segment), nvmeibt_praid_UUID(disk_segment->seg_mgmt.its_praid));
	}
	return rv;
}

void change_seg_index(struct nvmeibt_disk_segment *seg, int8_t idx)
{
	N_Tf(bhhdd72, "seg=@UUID_8 idx changed(@INT->@INT)",
		 nvmeibt_seg_UUID_8(seg), nvmeibt_disk_segment_idx_in_praid(seg), idx);
	seg->from_config.idx_in_praid = idx;
}

int nvmeibt_praid_validate_praids_config(void)
{
	struct nvmeibt_praid			*praid;
	int								rv = 0;
	int8_t							i, old_idx;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_praid_mgmt		*praid_mgmt;
	struct nvmeibt_disk_segment		*seg;

	NFIN;

	NVMEIB_HASH_FOREACH(praid, cur_topo->praids_hash_by_uuid) {
		if (nvmeibt_praid_is_conf_corrupted(praid)) {
			N_Tf(ru87tu5, "Skipping praid=@UUID_LE conf_corrupted", nvmeibt_praid_UUID(praid));
			continue;
		}
		if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(praid)) {
			N_Tf(ru87e81, "Skipping praid=@UUID_LE outdated", nvmeibt_praid_UUID(praid));
			continue;
		}
		praid_mgmt = &praid->praid_mgmt;

		// Check missing segments

		if (nvmeibt_praid_is_deprecated_in_config(praid)) {
			// The praid is in deletion, so it can be with 'holes'. In this case just squash it.
			praid_mgmt->n_topo_segs = squash_praid_segs(praid_mgmt->topo_segs, praid_mgmt->n_topo_segs);
			for (i = praid_mgmt->n_topo_segs - 1; i >= 0; --i) {
				seg = praid_mgmt->topo_segs[i];
				old_idx = seg->from_config.idx_in_praid;
				if (old_idx != i) {
					if (praid_mgmt->replacement_topo_segs[old_idx]) {
						praid_mgmt->replacement_topo_segs[i] = praid_mgmt->replacement_topo_segs[old_idx];
						change_seg_index(praid_mgmt->replacement_topo_segs[i], i);
					}
					change_seg_index(seg, i);
				}
			}
		}
		else {
			for (i = praid_mgmt->n_topo_segs - 1; i >= 0; --i) {
				if (!praid_mgmt->topo_segs[i]) {
					nvmeibt_praid_mark_conf_corrupted(praid);
					N_Ef(i8u7yb1, "praid=@UUID_LE, missing disk_segment #@INT", nvmeibt_praid_UUID(praid), i);
					rv = -1;
				}
			}
		}

		// Check pointers to praid

		for (i = praid_mgmt->n_topo_segs - 1; i >= 0; --i) {
			if (!praid_mgmt->topo_segs[i])
				continue;
			if (validate_praid_seg(praid, praid_mgmt->topo_segs[i]) < 0) {
				rv = -1;
			}
		}
	}

	NFOUT;
	return rv;
}

enum NVMEIBTC_DS_MODE calc_access_mode(struct nvmeibt_seg_lot *seg_lot)
{
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx = &seg_lot->seg_topo;
	enum NVMEIBTC_DS_MODE					seg_access_mode;
	bool 									is_dead_client_wise = !nvmeibt_register_is_seg_lot_registrable_topo_wise(seg_lot, NULL);

	if (is_dead_client_wise) {
		seg_access_mode = NVMEIBTC_DS_MODE_DEAD;
	} else if (nvmeibt_disk_segment_is_self_owner(seg_lot)) {
		seg_access_mode = NVMEIBTC_DS_MODE_RW;
#if 0 // this access mode is not used
	} else if (nvmeibt_disk_segment_is_competent_owner(seg_topo_ctx)) {	// Fully recovered, No need to turn off dirty bits
		seg_access_mode = NVMEIBTC_DS_MODE_W_NO_DIRTY;
#endif
	} else if (!nvmeibt_seg_topo_is_newly_added(seg_topo_ctx)) {
		seg_access_mode = NVMEIBTC_DS_MODE_W;
	} else {
		seg_access_mode = NVMEIBTC_DS_MODE_W_IS_DIRTY;
	}
	return seg_access_mode;
}

int nvmeibt_praid_lot_calc_topo_for_clients(struct nvmeibt_praid_lot *praid_lot,
											struct tTopoOfPraid *out_topo_for_clients,
											u8 io_perms_all,
											bool do_update_and_print)
{
	int8_t									i;
	int										rv = 1;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	struct nvmeibt_praid_topo_ctx			*praid_topo_ctx;
	struct nvmeibt_praid					*praid = praid_lot->my_praid;
	struct nvmeibt_seg_lot					*seg_lot;
	struct nvmeibt_seg_active				*seg_active;

	// FIN;
	praid_topo_ctx = &praid_lot->topo_ctx;
	if (do_update_and_print && out_topo_for_clients->header.praid_version == praid_topo_ctx->praid_version_major) {
		N_Tf(oli987y, "praid=@UUID_LE Ignoring, unchanged praid_version", nvmeibt_praid_UUID(praid));
		rv = 0;
		goto out;
	}

	// The topo_for_clients changes should go hand-in-hand with praid_version_major.
	// Note that the same praid_version_major can be activated/none, and we ignore it.

	if (do_update_and_print && (praid_topo_ctx->praid_version_major <= 0) && praid->was_praid_ever_activated) {
		N_Ef(bgay76a, "praid=@UUID_LE praid_version_major=@PRAID_VERSION", nvmeibt_praid_UUID(praid), praid_topo_ctx->praid_version_major);
	}
	// Prepare access modes
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		seg_lot = praid_lot->topo_seg_lots[i];
		seg_active = nvmeibt_disk_segment_get_seg_active(seg_lot->my_seg);
		NDUMP_N_ACTIVE_REGISTRANTS(dy77uw3, seg_active);

		nvmeibt_client_topo_disk_segment_write(&(out_topo_for_clients->s[i]),
											   nvmeibt_seg_lot_id_str(seg_lot),
											   calc_access_mode(seg_lot));
	}
	// Update the owners
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		seg_lot = praid_lot->topo_seg_lots[i];
		seg_topo_ctx = &seg_lot->seg_topo;
		if (nvmeibt_disk_segment_is_x(seg_topo_ctx)) {
			struct nvmeibt_client_topo_disk_segment     *seg_topo = &(out_topo_for_clients->s[i]);
			nvmeibt_client_reset_seg_topo_owners(seg_topo);
			if (do_update_and_print)
				N_Tf(jse43n7, "skipping seg=@UUID_8 is X", nvmeibt_seg_lot_UUID_8(seg_lot));
			continue;
		}
		if (do_update_and_print)
			N_Tf(u87y65c, "@UUID_8 owner=@UUID_8", nvmeibt_seg_lot_UUID_8(seg_lot), nvmeibt_seg_lot_UUID_8(seg_topo_ctx->owner_seg_lot));
		nvmeibt_client_topo_disk_segment_upd_owners(out_topo_for_clients->s,
													praid_lot->n_topo_seg_lots,
													nvmeibt_seg_lot_id_str(seg_topo_ctx->owner_seg_lot),
													nvmeibt_seg_lot_id_str(seg_topo_ctx->secondary_owner_seg_lot),
													praid_lot->from_config.lock_scheme_type, praid_lot->from_config.redundancy + 1, i);
	}
	nvmeibt_client_topo_praid_write(&(out_topo_for_clients->header),
									nvmeibt_praid_id_str(praid),
									praid_topo_ctx->praid_version_major,
									nvmeibt_praid_client_sync_blkset_sync_safety(praid_topo_ctx->registrants_sync_cmd),
									io_perms_all,
									praid_lot->n_topo_seg_lots);
	N_Tf(qw3ex09, "praid=@UUID_LE length=@LENGTH_INT", nvmeibt_praid_UUID(praid), nvmeibt_tTopoOfPraid_len(out_topo_for_clients));

	if (do_update_and_print) {	// Read the final topo_for_clients, and print it
		char						praid_uuid[40];
		int							praid_version;
		u8							blkset_sync_safety;
		union io_perms_bitfield		io_perms;
		int8_t						n_segments;

		nvmeibt_client_topo_praid_read(&(out_topo_for_clients->header), praid_uuid, sizeof(praid_uuid), &praid_version,
									   &blkset_sync_safety, &(io_perms.all), &n_segments);
		N_Tf(oplqw3x, "uuid=@UUID praid_version=@PRAID_VERSION blkset_sync_safety=@SYNC_SAFETY io_perms=@IO_PERMS n_segments=@N_SEGMENTS",
			praid_uuid, praid_version, blkset_sync_safety, io_perms.all, n_segments);
		for (i = 0; i < n_segments; i++) {
			struct nvmeibt_urn_uuid		seg_id;
			int		access_mode;
			char	uuid_of_primary_owner_segment[40];
			char	uuid_of_secondary_owner_segment[40];
//			struct nvmeibt_disk_segment		*seg;

			nvmeibt_client_topo_disk_segment_read(&(out_topo_for_clients->s[i]),
												  seg_id.str, sizeof(seg_id.str),
												  &access_mode,
												  uuid_of_primary_owner_segment, sizeof(uuid_of_primary_owner_segment),
												  uuid_of_secondary_owner_segment, sizeof(uuid_of_secondary_owner_segment));
			N_Tf(rtyu890,
				 "uuid=@UUID access_mode=@ACM uuid_of_primary_owner_seg=@DISK_SEGMENT_ID_STR ",
				 seg_id.str, nvmeibt_client_topo_seg_access_mode_to_str(access_mode), uuid_of_primary_owner_segment);
		}
	}
out:
	// FOUT;
	return rv;
}

static int leader_validate_registrants_sync_cmd(struct nvmeibt_praid *praid,
												enum PRAID_REGISTRANTS_SYNC_CMD new_sync_cmd,
												struct dirty_bits_state_stats *n)
{
	enum PRAID_REGISTRANTS_SYNC_CMD			baseline_sync_cmd;
	BOOL									is_legal = 1;
	int										i;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;

	NFIN;
	// Validate the transition from the baseline (sync_cmd) to the new calculated one
	baseline_sync_cmd = praid->praid_leader.baseline_praid_lot.topo_ctx.registrants_sync_cmd;
	if (baseline_sync_cmd == new_sync_cmd) {
		goto out;
	}
	// Sanity check, Leader. In some cases, further refinement is needed to validate the state transition per seg
	switch (new_sync_cmd) {
	case PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN:
		is_legal = 1;
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:
		is_legal = !!(baseline_sync_cmd &
			  (PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS |
			   PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_STABLE |
			   PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W |
			   PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D |
			   PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE | PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE));
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:
		is_legal = (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:
		is_legal = (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:
		is_legal = (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:
		is_legal = (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:
		is_legal = (!!(baseline_sync_cmd &
					   (PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE | PRAID_REGISTRANTS_SYNC_CMD_STABLE_I |
						PRAID_REGISTRANTS_SYNC_CMD_STABLE)) ||
					(baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS && n->x));	// Replacement seg is X_DONE
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE:
		is_legal =
			((baseline_sync_cmd & (PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE)) ||
			 (nvmeibt_praid_is_jbod(praid) && (baseline_sync_cmd & PRAID_REGISTRANTS_SYNC_CMD_STABLE_I)) ||
			 (n->is_degraded && (baseline_sync_cmd & (PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X))) ||
			 (nvmeibt_praid_is_using_reset_registrants_for_owner_change(praid) && (baseline_sync_cmd & PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS)));
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:	// An owner died on us
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:
		is_legal = 1;
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:	// A non-owner is going down / died
		is_legal = 0;
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			// Kind of heuristic. One of the pre-conditions is to have a non-owner
			if (!nvmeibt_seg_lot_is_owner_of_any_seg_in_baseline_topo(calculated_praid_lot->topo_seg_lots[i])) {
				is_legal = 1;
				break;
			}
		}
		is_legal &= !!(baseline_sync_cmd &
			  (PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W |
			   PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U | PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D |
			   PRAID_REGISTRANTS_SYNC_CMD_STABLE_I | PRAID_REGISTRANTS_SYNC_CMD_STABLE | PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS));
	break;
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:
		is_legal = !!(baseline_sync_cmd &
					  (PRAID_REGISTRANTS_SYNC_CMD_STABLE |
					   PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE |
					   PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE |
					   PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X));
		is_legal |= ((nvmeibt_praid_is_client_sync_cmd_switch_topo(baseline_sync_cmd) ||
					  (baseline_sync_cmd & PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS)) &&
					 n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty);
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I:
		is_legal = 1;	// We can start cold recovery regardless of the prev state
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R:
		is_legal = (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I);	// Re-init, since segs died/revived
		break;
	default:
		N_Ef(ytun448, "registrants_sync_cmd=@REGISTRANTS_SYNC_CMD", new_sync_cmd);
		break;
	}
	// A new leader can switch from unknown to any
	is_legal |= (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN);
	NTOMA_ASSERT(iwbs4jl, is_legal, "registrants_sync_cmd praid=@UUID_LE @PRAID_REGISTRANTS_SYNC_CMD_STR->@PRAID_REGISTRANTS_SYNC_CMD_STR",
				 nvmeibt_praid_UUID(praid), praid_registrants_sync_cmd_str(baseline_sync_cmd), praid_registrants_sync_cmd_str(new_sync_cmd));
out:
	NFOUT;
	return is_legal;
}

void nvmeibt_praid_lot_upd_registrants_sync_cmd(struct nvmeibt_praid_lot *praid_lot, enum PRAID_REGISTRANTS_SYNC_CMD registrants_sync_cmd)
{
	struct nvmeibt_praid				*praid = praid_lot->my_praid;
	struct nvmeibt_praid_topo_ctx		*topo_ctx;
	enum PRAID_REGISTRANTS_SYNC_CMD 	overriding_registrants_sync_cmd;
	bool								is_updating_leader_topo = (&praid->praid_leader.calculated_praid_lot == praid_lot);

	topo_ctx = &praid_lot->topo_ctx;
	// Trace args:[praid_id_str, is_updating_leader_topo, praid_registrants_sync_cmd_str, praid_registrants_sync_cmd_str]
	N_Tf(fhuyt7r, "praid=@UUID_LE registrants_sync_cmd (@IS_UPDATING_LEADER_TOPO) @PRAID_REGISTRANTS_SYNC_CMD_STR->@PRAID_REGISTRANTS_SYNC_CMD_STR",
		 nvmeibt_praid_UUID(praid), (is_updating_leader_topo ? 'L' : 'A'),
		 praid_registrants_sync_cmd_str(topo_ctx->registrants_sync_cmd),
		 praid_registrants_sync_cmd_str(registrants_sync_cmd));
	if (topo_ctx->registrants_sync_cmd == registrants_sync_cmd) {
		goto out;
	}
	// DELETE is sticky - no next state
	if (nvmeibt_praid_is_client_sync_cmd_delete(topo_ctx->registrants_sync_cmd)) {	TODO(is this case needed)
		N_Tf(ft438ki, "Rejecting @STR. Already in DELETE", praid_registrants_sync_cmd_str(registrants_sync_cmd));
		goto out;
	}
	// If we required complete UNREG, then do not transition to a state that doesn't require UNREG
	// Note that a new leader started from fresh (reset of leader_topo_ctx).
	if (	is_updating_leader_topo &&
			!nvmeibt_praid_is_client_sync_cmd_unknown(registrants_sync_cmd) &&	// Unknown is set upon new leader's reset
			!nvmeibt_praid_is_client_sync_cmd_req_unregister(registrants_sync_cmd) &&
			nvmeibt_praid_is_client_sync_cmd_req_unregister(topo_ctx->registrants_sync_cmd) &&
			!(topo_ctx->leader_did_all_segs_sync_registrants)) {
		overriding_registrants_sync_cmd =
			(nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(registrants_sync_cmd) ? PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I : PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS);
		TODO(How does it handle _I->sw_topo_x, for now converting to RESET_REGISTRANTS);
		N_Tf(agku87w, "@PRAID_REGISTRANTS_SYNC_CMD_STR is stronger than @PRAID_REGISTRANTS_SYNC_CMD_STR. Setting @PRAID_REGISTRANTS_SYNC_CMD_STR",
			praid_registrants_sync_cmd_str(topo_ctx->registrants_sync_cmd), praid_registrants_sync_cmd_str(registrants_sync_cmd), praid_registrants_sync_cmd_str(overriding_registrants_sync_cmd));
		registrants_sync_cmd = overriding_registrants_sync_cmd; // Do not mix old and new registrants
	}
	// accept the new value
	if (topo_ctx->registrants_sync_cmd != registrants_sync_cmd) {
		topo_ctx->registrants_sync_cmd = registrants_sync_cmd;
		topo_ctx->leader_did_all_segs_sync_registrants = !nvmeibt_praid_is_client_sync_cmd_req_client_ack(registrants_sync_cmd);
	}
out:
	NFOUT;
}

static void praid_leader_check_whether_all_segments_registrants_are_aligned(struct nvmeibt_praid *praid)
{
	int										i;
	BOOL									are_sync = 1;
	BOOL									is_praid_version_major_matching;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*remote_seg_topo;

	NFIN;
	if (nvmeibt_praid_is_client_sync_cmd_req_client_ack(calculated_praid_topo->registrants_sync_cmd)) {
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
			is_praid_version_major_matching = (calculated_praid_topo->praid_version_major == remote_seg_topo->seg_praid_version_major);
			N_Tf(6cbhs73, "seg=@UUID_8 @DIRTY_BITS_STATE_STR is_synchronizer=@IS_SYNCHRONIZER are_registrants_synced=@ARE_REGISTRANTS_SYNCED is_praid_version_major_matching=@IS_PRAID_VERSION_MAJOR_MATCHING",
				nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
				dirty_bits_state_str(calculated_seg_topo->dirty_bits_state),
				calculated_seg_topo->is_registrants_synchronizer,
				remote_seg_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd,
                is_praid_version_major_matching);
			if (!calculated_seg_topo->is_registrants_synchronizer) {
				continue;
			}
			if (!remote_seg_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd || !is_praid_version_major_matching) {
				are_sync = 0;
				break;
			}
		}
	}
	if (calculated_praid_topo->leader_did_all_segs_sync_registrants != are_sync) {
		calculated_praid_topo->leader_did_all_segs_sync_registrants = are_sync;
		N_Tf(bcur4no, "are_sync=@ARE_SYNC", are_sync);
	}
	NFOUT;
}

static void serialize_seg_lot_topo_to_wire(struct nvmeibt_praid_topo_ctx *praid_topo,
											  struct nvmeibt_seg_lot *seg_lot,
											  struct nvmeibt_serialized_seg_leader_topo *wire_topo_ptr)
{
	struct nvmeibt_serialized_seg_leader_topo 		serialized = {0};	// Specifically, zero the unused eyecatcher and seg_idx
	struct nvmeibt_disk_segment_topo_ctx			*seg_topo;

	NFIN;
	if (!nvmeibt_seg_lot_is_config_OK(seg_lot) && !nvmeibt_seg_lot_is_deleted_in_config(seg_lot)) {
		goto out;
	}
	seg_topo = &seg_lot->seg_topo;
	nvmeibt_strlcpy(serialized.eyecatcher, "STO", sizeof(serialized.eyecatcher));
	serialized.uuid = *nvmeibt_seg_UUID(seg_lot->my_seg);
	serialized.owner_idx = seg_topo->owner_seg_lot ? seg_topo->owner_seg_lot->from_config.idx_in_praid : DUMMY_OWNER;
	serialized.secondary_owner_idx = seg_topo->secondary_owner_seg_lot ? seg_topo->secondary_owner_seg_lot->from_config.idx_in_praid : DUMMY_OWNER;

	serialized.dirty_bits_state = seg_topo->dirty_bits_state;
	serialized.praid_version_major = praid_topo->praid_version_major;
	serialized.praid_version_minor = praid_topo->praid_version_minor;

	serialized.dirty_bits_init_mode = seg_topo->dirty_bits_init_mode;
	serialized.stale_locks_init_mode = seg_topo->stale_locks_init_mode;
	serialized.txid_init_mode = serialized.dirty_bits_init_mode; // for backward compatibility only, can be deleted in future versions
	serialized.is_registrants_synchronizer = seg_topo->is_registrants_synchronizer;
	serialized.leader_seg_flags = seg_topo->leader_seg_flags;
	// send_topo_ptr->res_1 = 0;
	// send_topo_ptr->res_2 = 0;
	nvmeibt_disk_segment_convert_topo_le_be(&serialized, wire_topo_ptr);

out:
	NFOUT;
}

static void praid_leader_serialize_topo(struct nvmeibt_praid *praid)
{
	struct nvmeibt_seg_lot							*seg_lot;
	struct nvmeibt_praid_serialized_topo			serialized_praid = {0};
	struct nvmeibt_serialized_seg_leader_topo		*seg_wire_topo_ptr;
	int												segs_topo_len;

	struct nvmeibt_praid_leader						*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot						*praid_lot = &praid_leader->baseline_praid_lot;
	struct nvmeibt_praid_topo_ctx					*praid_topo = &praid_lot->topo_ctx;

	NFIN;
	// The design says that a praid that was re-calculated by the leader
	//  needs reserialization, but praids that are not yet calculated should
	//  be distributed as-is from the global-topo that the leader uses as
	//  its starting point.
	// Note that we might serialize the old topo with a higher version_minor, so
	//  that the leader knows for sure that a received APPLIED_DISK_SEGMENTs
	//  refer to the topology that it distributed.

	if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(praid)) {
		NNVMEIBT_BUF_FREE(tcvsjjk, &(praid_leader->segs_wire_topo_buf));
		memset(&(praid_leader->praid_wire_topo), 0, sizeof(praid_leader->praid_wire_topo));
		goto out;
	}
	if ((praid_leader->serialized_version_major == praid_topo->praid_version_major &&
		 praid_leader->serialized_version_minor == praid_topo->praid_version_minor) &&
		nvmeibt_praid_is_serialized(praid)) {
		N_Tf(jgii982, "Skipping. Same praid_version");
		goto out;
	}
	if (!(praid_topo->is_activated) && !(praid->was_praid_ever_activated)) {
		goto out;	// Start the persistency life-cycle only after the first activation ever
	}
	if (nvmeibt_praid_is_conf_corrupted(praid)) {
		N_Tf(uy76t5r, "Skipping praid=@UUID_LE. conf_corrupted", nvmeibt_praid_UUID(praid));
		goto out;
	}
	// For non-activated praid (e.g., no owner found, use the last activated states)
	// This means that on leader startup, it immediately distributes and asks to apply the
	//  latest global topology (including the old is_activated).
	//  Once it has new inputs it will calculate a new leader_topo, and send the new is_activated.

	nvmeibt_strlcpy(serialized_praid.eyecatcher, "PTO", sizeof(serialized_praid.eyecatcher));
	serialized_praid.uuid = *nvmeibt_praid_UUID(praid);
	serialized_praid.praid_version_major = praid_topo->praid_version_major;
	serialized_praid.praid_version_minor = praid_topo->praid_version_minor;
	serialized_praid.registrants_sync_cmd = praid_topo->registrants_sync_cmd;
	serialized_praid.leader_did_all_segs_sync_registrants = praid_topo->leader_did_all_segs_sync_registrants;
	serialized_praid.is_activated = praid_topo->is_activated;
	serialized_praid.segs_num = XDLIST_N_ELEMNTS(&praid_lot->all_seg_lot_list);
	// Since we may serialize a praid without updating it, topo_idx_updated may not equal to leader_get_next_topology_version().
	serialized_praid.topo_idx_updated = praid_topo->topo_idx_updated;
	// send_topo_ptr->res_1 = 0;
	// send_topo_ptr->res_2 = 0;
	// send_topo_ptr->res_3 = 0;

	if (serialized_praid.topo_idx_updated != leader_get_next_topology_version()) {
		N_Tf(ajkld10, "Serializing a praid updated in @INT64_TX while calculating topology @INT64_TX.", serialized_praid.topo_idx_updated, leader_get_next_topology_version());
	}

	// disk_segments
	segs_topo_len = sizeof(struct nvmeibt_serialized_seg_leader_topo) * XDLIST_N_ELEMNTS(&praid_lot->all_seg_lot_list);
	NNVMEIBT_BUF_RESIZE(thyujq0, &(praid_leader->segs_wire_topo_buf), (size_t)segs_topo_len);
	memset(praid_leader->segs_wire_topo_buf.data_buf, 0, praid_leader->segs_wire_topo_buf.buf_len);

	seg_wire_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)praid_leader->segs_wire_topo_buf.data_buf;
	XDLIST_FOREACH(seg_lot, &praid_lot->all_seg_lot_list) {
		serialize_seg_lot_topo_to_wire(praid_topo, seg_lot, seg_wire_topo_ptr);
		seg_wire_topo_ptr++;
	}
	SET_RAFT_LEADER_NEXT_TOPOLOGY_VERSION(cbhj34k);
	nvmeibt_praid_convert_topo_le_be(&serialized_praid, &(praid_leader->praid_wire_topo), TOMA_SW_COMPATIBILITY_VER);
	nvmeibt_praid_print_leader_wire_topo_with_segs(praid_leader);
out:
	praid_leader->serialized_version_major = praid_topo->praid_version_major;
	praid_leader->serialized_version_minor = praid_topo->praid_version_minor;
	NFOUT;
}

void mm_segment_conf_from_seg(struct nvmeibt_seg_lot *seg_lot, struct mm_segment_conf *send_topo_ptr)
{
	struct nvmeibt_disk_segment_config		*f = &(seg_lot->from_config);
	struct nvmeibt_disk_segment				*seg = seg_lot->my_seg;

	NFIN;

	nvmeibt_strlcpy(send_topo_ptr->eyecatcher, "SEG", sizeof(send_topo_ptr->eyecatcher));
	send_topo_ptr->uuid = *nvmeibt_seg_UUID(seg);
	send_topo_ptr->diskUUID = seg->seg_mgmt.disk_id;
	send_topo_ptr->lbs = seg->seg_mgmt.lb_s;
	send_topo_ptr->lbe = seg->seg_mgmt.lb_e;
	send_topo_ptr->pRaidIndex = f->idx_in_praid;
	send_topo_ptr->pRaidTypeIndex = 0; // f->idx_in_praid_role; obsolete
	send_topo_ptr->type = 1; // 0 - obsolete raft_only
	send_topo_ptr->action = f->deprecation_flag;
	NFOUT;
}

static void leader_generate_topo_config_buf_of_praid_and_its_segs_mm_conf_from_baseline_praid_lot(struct nvmeibt_praid *praid)
{
	struct nvmeibt_praid_leader						*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot						*praid_lot = &praid_leader->baseline_praid_lot;
	struct nvmeibt_seg_lot							*seg_lot;
	struct nvmeibt_praid_config						*f = &(praid_lot->from_config);
	struct mm_praid_conf							*praid_conf;
	struct mm_segment_conf							*seg_conf_ptr;
	unsigned int									segs_conf_len;
	int8_t											n_segs;
	int8_t											n_segs_serialized = 0;

	NFIN;

	praid_conf = &praid_leader->serialized_topo_config_praid_and_its_segs_arr_conf;
	nvmeibt_strlcpy(praid_conf->eyecatcher, "PRD", sizeof(praid_conf->eyecatcher));
	praid_conf->uuid = *nvmeibt_praid_UUID(praid);
	praid_conf->activated = f->was_ever_activated;
	praid_conf->stripeIndex = praid->praid_mgmt.stripe_idx;
	praid_conf->version = f->version;
	praid_conf->topo_config_idx_updated = f->topo_config_idx_updated; // Copy the topo_config_idx_updated from runtime structure.

	n_segs = XDLIST_N_ELEMNTS(&praid_lot->all_seg_lot_list);
	praid_conf->num_segments = n_segs;
	if (praid_conf->num_segments == 0) {
		N_Wf(ygadflb, "praid=@UUID_LE num_segments=@INT", &(praid_conf->uuid), praid_conf->num_segments);
	}
	segs_conf_len = sizeof(struct mm_segment_conf) * n_segs;
	NNVMEIBT_BUF_RESIZE(dhu7619, &(praid_leader->topo_config_array_of_its_serialized_segs_conf), segs_conf_len);
	memset(praid_leader->topo_config_array_of_its_serialized_segs_conf.data_buf, 0, praid_leader->topo_config_array_of_its_serialized_segs_conf.buf_len);

	seg_conf_ptr = (struct mm_segment_conf *)praid_leader->topo_config_array_of_its_serialized_segs_conf.data_buf;
	praid_conf->segments = seg_conf_ptr;

	XDLIST_FOREACH(seg_lot, &praid_lot->all_seg_lot_list) {
		mm_segment_conf_from_seg(seg_lot, seg_conf_ptr);
		seg_conf_ptr++;
		n_segs_serialized++;
	}
	NTOMA_ASSERT(cfwk5xn, n_segs_serialized == n_segs, "n_segs_serialized=@INT n_segs=@INT", n_segs_serialized, n_segs);
	nvmeibt_mm_jason_leader_topo_config_mm_praid_conf_and_mm_segs_conf_to_wire_buf(praid);
//out:
	NFOUT;
}

// Upd committed topo on startup (from persist) or when received from the leader
enum nvmeibt_add_rv nvmeibt_praid_upd_committed_topo(struct nvmeibt_praid_serialized_topo *praid_topo_ptr)
{
	enum nvmeibt_add_rv				rv = NVMEIBT_ADD_FAILED;
	struct nvmeibt_praid			*praid;
	struct nvmeibt_praid_topo_ctx	*committed_topo;
	BOOL							is_same_praid_version;

	NFIN;

	praid = nvmeibt_praid_get_praid_by_id(&praid_topo_ptr->uuid);
	if (!praid) {
		if (nvmeibt_praid_is_client_sync_cmd_delete(praid_topo_ptr->registrants_sync_cmd)) {
			rv = NVMEIBT_ADD_SKIPPED;
		}
		else {
			rv = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;
			N_Ef(drt65t2, "Surprise delete praid=@UUID_LE. Existed in the old topo (not X), but does not exist in config.", &(praid_topo_ptr->uuid));
		}
		goto out;
	}

	committed_topo = &praid->praid_follower.committed_praid_lot.topo_ctx;
	is_same_praid_version = (praid_topo_ptr->praid_version_major == committed_topo->praid_version_major) &&
							(praid_topo_ptr->praid_version_minor == committed_topo->praid_version_minor);
	if (is_same_praid_version) {
		rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
		goto out_ok;
	}
	N_Tf(nhy76cw, "praid=@UUID_LE praid_version=@PRAID_VERSION,@PRAID_VERSION(@PRAID_VERSION,@PRAID_VERSION) is_activated=@IS_ACTIVATED(@IS_ACTIVATED) topo_idx_updated=@INT64_TX(@INT64_TX)",
		nvmeibt_praid_UUID(praid),
		praid_topo_ptr->praid_version_major, praid_topo_ptr->praid_version_minor,
		committed_topo->praid_version_major, committed_topo->praid_version_minor,
		praid_topo_ptr->is_activated, committed_topo->is_activated, praid_topo_ptr->topo_idx_updated, committed_topo->topo_idx_updated);

	memset(committed_topo, 0, sizeof(*committed_topo));
	committed_topo->praid_version_major = praid_topo_ptr->praid_version_major;
	committed_topo->praid_version_minor = praid_topo_ptr->praid_version_minor;
	committed_topo->registrants_sync_cmd = praid_topo_ptr->registrants_sync_cmd;
	committed_topo->leader_did_all_segs_sync_registrants = praid_topo_ptr->leader_did_all_segs_sync_registrants;
	committed_topo->is_activated = praid_topo_ptr->is_activated;
	committed_topo->topo_idx_updated = praid_topo_ptr->topo_idx_updated;
	praid->was_praid_ever_activated |= praid_topo_ptr->is_activated;

	rv = NVMEIBT_ADD_MODIFIED;
out_ok:
	NVMEIBT_PRAID_TOPO_DUMP(5vvwsu7, nvmeibt_praid_UUID(praid), "Committed", committed_topo);
out:
	NFOUT;
	return rv;
}

static void praid_lot_reset_registrants_sync_status(struct nvmeibt_praid_lot *praid_lot)
{
	int									i;

	NFIN;
	// Remove traces of synchronization that were read from the global topology
	praid_lot->topo_ctx.leader_did_all_segs_sync_registrants = 0;
	nvmeibt_praid_lot_upd_registrants_sync_cmd(praid_lot, PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN);
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		praid_lot->topo_seg_lots[i]->seg_topo.active_seg_flags.are_praid_registrants_aligned_with_sync_cmd = 0;
		praid_lot->topo_seg_lots[i]->seg_topo.is_registrants_synchronizer = 0;
	}
	NFOUT;
}

static void praid_lot_reset_topo_ctx(struct nvmeibt_praid_lot *praid_lot)
{
	struct nvmeibt_praid_topo_ctx			*praid_topo = &praid_lot->topo_ctx;

	NFIN;
	memset(praid_topo, 0, sizeof(struct nvmeibt_praid_topo_ctx));
	praid_topo->praid_version_major = PRAID_VERSION_INITIAL_VALUE;
	praid_topo->praid_version_minor = PRAID_VERSION_INITIAL_VALUE;
	praid_topo->is_activated = 0;
	praid_topo->topo_idx_updated = nvmeibt_offset_and_idx_uninitialized;  // -1 means never updated
	praid_topo->registrants_sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN;
	// praid_lot_reset_registrants_sync_status(praid_lot);
	NFOUT;
}

static void praid_lot_one_time_init(struct nvmeibt_praid_lot *praid_lot, struct nvmeibt_praid *praid, int seg_lot_offset)
{
	//praid_lot->from_config = praid->from_config;
	praid_lot->from_config.version = ILLEGAL_CONFIG_VER; // Cause upd_from_praid_mgmt to do the work
	praid_lot->my_praid = praid;
	praid_lot->my_seg_lot_offset = seg_lot_offset;
	XDLIST_HEAD_INIT(&(praid_lot->all_seg_lot_list));
	praid_lot_reset_topo_ctx(praid_lot);
}

#define GET_SEG_LOT_OFFSET(_seg_lot)		offsetof(struct nvmeibt_disk_segment, _seg_lot)

static void praid_one_time_init(struct nvmeibt_praid *praid)
{
	NFIN;
	NVMEIBT_BUF_INIT(&praid->praid_leader.topo_config_array_of_its_serialized_segs_conf);
	NVMEIBT_BUF_INIT(&praid->praid_leader.segs_wire_topo_buf);
	praid->praid_leader.praid_wire_topo.segs_num = LE_SWAP8(ILLEGAL_SEGS_NUM); // Mark as never serialized
	praid_lot_one_time_init(&praid->praid_leader.baseline_praid_lot, praid, GET_SEG_LOT_OFFSET(seg_leader.baseline_seg_lot));
	praid_lot_one_time_init(&praid->praid_leader.calculated_praid_lot, praid, GET_SEG_LOT_OFFSET(seg_leader.calculated_seg_lot));
	praid_lot_one_time_init(&praid->praid_leader.to_report_praid_lot, praid, GET_SEG_LOT_OFFSET(seg_leader.to_report_seg_lot));
	praid_lot_one_time_init(&praid->praid_follower.committed_praid_lot, praid, GET_SEG_LOT_OFFSET(seg_follower.committed_seg_lot));
	praid_lot_one_time_init(&praid->praid_follower.applied_praid_lot, praid, GET_SEG_LOT_OFFSET(seg_follower.applied_seg_lot));
//	leader_generate_topo_config_buf_of_praid_and_its_segs_mm_conf_from_baseline_praid_lot(praid);
	if (nvmeibt_praid_is_jbod(praid)) {
		praid->praid_leader.is_waiting_for_timeout_since_activation_attempt = 0;
	}
	else {
		praid->praid_leader.is_waiting_for_timeout_since_activation_attempt = 1;
		praid->praid_follower.applied_praid_lot.topo_ctx.praid_version_major = -1;
	}
	// Now that we know its blkdev, generate vol_name_for_recovery
	snprintf(praid->vol_name_for_recovery, sizeof(praid->vol_name_for_recovery), "%.17s[%02x.%01x]%.8s",	// Fit in null terminated 32 chars
			 nvmeibt_praid_get_blkdev_name(praid),
			 praid->praid_mgmt.its_chunk->its_idx_in_block_device % 0x100, praid->praid_mgmt.stripe_idx % 0x10,
			 nvmeibt_praid_id_str(praid));
	praid->vol_uuid_for_recovery = *nvmeibt_praid_UUID(praid);
	praid->vol_uuid_for_recovery.ll[1] ^= 0xdeadbeefdeadbeefLL;
	NFOUT;
}

void nvmeibt_praid_reset_due_to_convert_to_leader(struct nvmeibt_praid *praid)
{
	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;

	NFIN;
	praid_leader->previous_topo_for_clients.header.n_segments = -1;
	praid_leader->did_any_client_report_about_problems = 0;
	praid_leader->praid_wire_topo.segs_num = LE_SWAP8(ILLEGAL_SEGS_NUM); // Mark a never serialized topo
	getnstimeofday_boot(&(praid_leader->last_serialization_timestamp));	// Wait for non-responsive segs as if we serialized now

	//nvmeibt_praid_lot_duplicate_content(&praid_leader->baseline_praid_lot, &praid->praid_follower.committed_praid_lot);
	nvmeibt_praid_leader_we_have_a_new_baseline(praid, &praid->praid_follower.committed_praid_lot);
	NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(mna1213, praid);
	NFOUT;
}

static void increase_praid_topo_version(struct nvmeibt_praid_topo_ctx *praid_topo, BOOL is_clients_synchronization_required)
{
	NFIN;
	if (is_clients_synchronization_required) {
		++(praid_topo->praid_version_major);	// Increase praid_version_major
		praid_topo->praid_version_minor = 0;	// And reset the minor
	}
	else {
		++(praid_topo->praid_version_minor);	// Increase praid_version_minor
	}
	praid_topo->topo_idx_updated = leader_get_next_topology_version();	// Increased praid version will generate a new baseline, and trigger serialization in which TOPO leader_calculated will be updated to leader_get_next_topology_version(). That value will be the version of this new baseline.
	NFOUT;
}

static void praid_leader_calc_praid_version_and_registrants_sync_cmd(struct nvmeibt_praid *praid,
																	 struct dirty_bits_state_stats *n,
																	 bool is_delete,
																	 bool is_topo_for_clients_different,
																	 bool was_io_R_or_W_permitted,
																	 bool is_praid_topo_any_different,
																	 bool is_cold_recovery_done_now)
{
	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;
	struct nvmeibt_praid_lot				*baseline_praid_lot = &praid_leader->baseline_praid_lot;

	bool									is_clients_synchronization_required = 0;
	bool									did_all_segs_sync_registrants;
	enum PRAID_REGISTRANTS_SYNC_CMD 		baseline_sync_cmd = baseline_praid_lot->topo_ctx.registrants_sync_cmd;
	enum PRAID_REGISTRANTS_SYNC_CMD 		sync_cmd = baseline_sync_cmd;
	int										i;
#ifdef TOMA_DEBUG
	bool									is_synchronizer_found = 0;
#endif

	NFIN;
	did_all_segs_sync_registrants = calculated_praid_topo->leader_did_all_segs_sync_registrants;
	N_Tf(iu87v2, "vol=@VOL praid=@UUID_LE owner_died=@OWNER_DIED non_owner_died=@NON_OWNER_DIED under_recovery_I=@UNDER_RECOVERY_I under_recovery=@UNDER_RECOVERY_R is_degraded=@IS_DEGRADED "
		"is_bringup_after_lose_mem=@IS_BRINGUP_AFTER_LOSE_MEM is_delete=@IS_DELETE init_due_now=@INIT_DUE_NOW",
		nvmeibt_praid_get_blkdev_name(praid), nvmeibt_praid_UUID(praid),
		n->owner_died, n->non_owner_died, n->under_recovery_I, n->under_recovery_R, n->is_degraded,
		n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty, is_delete, n->init_due_now);
	if (is_delete) {
		sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_DELETE;
		goto calc_synchronizers;
	}
	if (n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty) {
		// EC-3306: The assumption is that we first restore the pre-crash config&topo state, and only then take config changes.
		// Supposedly, all the clients already unregistered and are not during I/O anyhow. No need to sync them
		// but, it might be that it was a network disconnect, and they are still alive, with registered clients ...
		sync_cmd = (n->is_cold_recovery_init_needed ? PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I :
					n->under_recovery_I ? PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I : PRAID_REGISTRANTS_SYNC_CMD_STABLE_I);
		goto calc_synchronizers;
	}

	if (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I) {
		if (n->init_due_now == 0) {
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R;
		}
		goto calc_synchronizers;
	}
	if (baseline_sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R) {
		if (is_cold_recovery_done_now) {
			N_Tf(iemksu4, "After COLD recovery we require full reregister");
			TODO(Consider SW_TOPO if hot dirty rebuild is next - already UNDER_RECOVERY_I/R);
			// Next we might start a hot-recovery
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS;
		}
		goto calc_synchronizers;
	}

	// Calc the next registrants_sync_cmd
	if (n->owner_died) {
		TODO(Replace with STABLE_DEGRADED_I);
		sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS;
		goto calc_synchronizers;
	}
	if (n->non_owner_died) {	// && (n->owner_died == 0)
		sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X;
		goto calc_synchronizers;
	}
	if (n->under_recovery_I) {
		sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I;
		goto calc_synchronizers;
	}
	// The ordinary states transitions. (Not Unreg / first-time)
	switch (baseline_sync_cmd) {
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I:
		if ((n->init_due_now == 0) && did_all_segs_sync_registrants) {
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W;
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W:
		if (did_all_segs_sync_registrants) {
			// Now we can start recovery
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U;
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U:
		if (n->under_recovery_R == 0) {	// Are we done with the UNDER_RECOVERY_R
			// Switch to dual owner
			sync_cmd = (nvmeibt_praid_is_using_reset_registrants_for_owner_change(praid) ?
						PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS :	// For now in EC, we avoid the owner-change using SW_TOPOs
						PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D);
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D:
		if (did_all_segs_sync_registrants) {
			// Switch to normal
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE;
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE:
		if (did_all_segs_sync_registrants) {
			sync_cmd = (n->is_degraded ? PRAID_REGISTRANTS_SYNC_CMD_STABLE_I : PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE);
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE:
		if (did_all_segs_sync_registrants) {
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_STABLE;
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X:
		if (did_all_segs_sync_registrants) {
			if (n->under_recovery_I) {
				N_Wf(w556aj8, "n->under_recovery_I should have been filtered earlier (no harm)");
				sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I;
			}
			else {
				if (n->is_degraded) {
					sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_STABLE_I;
				}
				else {
					N_Wf(ak998xf, "all_segs_sync_registrants. No dead & No under_recovery following SWITCH_TOPO_X.");
					sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X;	// Stuck
				}
			}
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS:
		if (did_all_segs_sync_registrants) {
			TODO(I suspect that the case of STABLE_NORMAL after UNREGISTER does not exist for non-EC);
			sync_cmd = (n->is_degraded ? PRAID_REGISTRANTS_SYNC_CMD_STABLE :
						n->under_recovery_I ? PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I :
						n->under_recovery_R ? PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W :	// All is ready to start the dirty-rebuild
						PRAID_REGISTRANTS_SYNC_CMD_STABLE	/* normal */);
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE_I:
		if (n->init_due_now == 0) {
			if (nvmeibt_praid_is_jbod(praid)) {
				sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_STABLE;
			} else {
				// We want to go to NORMAL, but since this change affects the clients
				//  (regenerate_topo_for_clients, open_to_registration), we want
				//  to transition through a topology that requires_client_ack
				sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE;
			}
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_STABLE:
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN:
		if (!(praid->was_praid_ever_activated)) {
			sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_STABLE_I;
		}
		break;
	case PRAID_REGISTRANTS_SYNC_CMD_DELETE:
		break;
	default:
		N_Ef(dgt6385, "registrants_sync_cmd=@REGISTRANTS_SYNC_CMD", baseline_sync_cmd);
		break;
	}
calc_synchronizers:
	is_clients_synchronization_required = (is_topo_for_clients_different ||
										   ((sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I) &&
											(baseline_sync_cmd != PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I)));
	if (is_clients_synchronization_required && was_io_R_or_W_permitted && (sync_cmd == PRAID_REGISTRANTS_SYNC_CMD_STABLE)) {
		// If praid major version will be changed we must synchronize registrants in any case
		// if !was_io_R_or_W_permitted, then nobody is registered for I/O, so there is no need to SW_TOPO. Go directly to STABLE.
		sync_cmd = PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE;
	}

	leader_validate_registrants_sync_cmd(praid, sync_cmd, n);
	nvmeibt_praid_lot_upd_registrants_sync_cmd(calculated_praid_lot, sync_cmd);	// Note upd_regs_sync_cmd() might decide to assign a value!=sync_cmd
	is_praid_topo_any_different |= (calculated_praid_topo->registrants_sync_cmd != baseline_sync_cmd);

	if (is_praid_topo_any_different) {
		if (is_clients_synchronization_required) {
			// Make all owners synchronizers
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				struct nvmeibt_disk_segment_topo_ctx	*seg_topo = &calculated_praid_lot->topo_seg_lots[i]->seg_topo;
				if (nvmeibt_disk_segment_is_competent_owner(seg_topo)) {
					seg_topo->is_registrants_synchronizer = 1;
#ifdef TOMA_DEBUG
					is_synchronizer_found = 1;
#endif
				}
			}
		}
		else {
			// If there are no changes in topo for clients we don't need synchronization
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				calculated_praid_lot->topo_seg_lots[i]->seg_topo.is_registrants_synchronizer = 0;
			}
		}
		increase_praid_topo_version(calculated_praid_topo, is_clients_synchronization_required);

#ifdef TOMA_DEBUG
		if (	calculated_praid_topo->is_activated &&
				is_clients_synchronization_required &&
				!is_synchronizer_found && nvmeibt_praid_is_client_sync_cmd_req_client_ack(calculated_praid_topo->registrants_sync_cmd)) {
			if (!nvmeibt_praid_is_client_sync_cmd_delete(calculated_praid_topo->registrants_sync_cmd)) {
				N_Ef(bhy12s7, "praid=@UUID_LE. No synchronizer, although activated and requires client_ack", nvmeibt_praid_UUID(praid));
				nvmeibt_abort(ES_FATAL);
			}
		}
#endif	// #ifdef TOMA_DEBUG
	}
	NFOUT;
}

static struct nvmeibt_seg_lot *find_other_owner_in_praid_lot(struct nvmeibt_praid_lot *praid_lot, int8_t idx_in_praid)
{
	int8_t							owner_no;
	struct nvmeibt_seg_lot			*candidate;
	struct nvmeibt_seg_lot			*owner = NULL;
	int8_t							candidate_idx;

	NFIN;
	// Find a owner, other than "self" (start from 1). Might be that there is none
	for (owner_no = 1; owner_no <= praid_lot->from_config.redundancy; owner_no++) {
		candidate_idx = get_owner_idx_by_owner_scheme_type(owner_no, praid_lot->from_config.lock_scheme_type, idx_in_praid, praid_lot->n_topo_seg_lots);
		if (candidate_idx == -1) {
			break;	// Error
		}
		candidate = praid_lot->topo_seg_lots[candidate_idx];
		if ((candidate_idx != idx_in_praid) && nvmeibt_disk_segment_is_competent_owner(&candidate->seg_topo)) {
			owner = candidate;
			break;
		}
	}
	NFOUT;
	return owner;
}

static void validate_owners(struct nvmeibt_praid *praid)
{
	int										i;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo;
	struct nvmeibt_seg_lot					*its_owner;
	struct nvmeibt_seg_lot					*its_secondary_owner;
	struct nvmeibt_disk_segment_topo_ctx	*owner_topo;

	NFIN;
	if (nvmeibt_praid_is_deprecated_in_config(praid))
		goto out;

	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		seg_topo = &calculated_seg_lot->seg_topo;
		if (nvmeibt_disk_segment_is_x(seg_topo))
			continue;

		its_owner = seg_topo->owner_seg_lot;
		its_secondary_owner = seg_topo->secondary_owner_seg_lot;
		owner_topo = &its_owner->seg_topo;

		if (nvmeibt_disk_segment_is_de_facto_owner(owner_topo) &&
			(!its_secondary_owner || nvmeibt_disk_segment_is_de_facto_owner(&its_secondary_owner->seg_topo))) {
			continue;
		}
		N_Ef(xierm8s, "seg=@UUID_8 owner=@UUID_8(@STR)", nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
			 nvmeibt_seg_lot_UUID_8(its_owner), nvmeibt_seg_topo_dirty_bits_state_str(owner_topo));
		if (its_secondary_owner)
			N_Ef(hy7wi82, "seg=@UUID_8 secondary_owner=@UUID_8(@STR)", nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
				 nvmeibt_seg_lot_UUID_8(its_secondary_owner), nvmeibt_seg_topo_dirty_bits_state_str(&its_secondary_owner->seg_topo));
		nvmeibt_abort(ES_FATAL);
	}
out:
	NFOUT;
}

static void calc_owners_per_disk_segment(struct nvmeibt_praid *praid)
{
	int										i;
	BOOL									is_owned_by_valid_owner;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	struct nvmeibt_seg_lot					*its_owner;

	NFIN;
	// Set the owner per segment.
	// If we keep the old owner, then clients' sync is not required
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		calculated_seg_topo = &calculated_seg_lot->seg_topo;

		its_owner = calculated_seg_topo->owner_seg_lot;
		is_owned_by_valid_owner = (its_owner && nvmeibt_disk_segment_is_competent_owner(&(its_owner->seg_topo)));

		if (nvmeibt_disk_segment_is_x(calculated_seg_topo)) {
			// There are no I/Os to a seg during deletion, so the seg doesn't need owners
			calculated_seg_topo->owner_seg_lot =  NULL;
			calculated_seg_topo->secondary_owner_seg_lot =  NULL;
			continue;
		}
		if (nvmeibt_disk_segment_is_de_facto_owner(calculated_seg_topo)) {
			NNVMEIBT_SEG_LOT_SET_OWNER(qnb7651, calculated_seg_lot, calculated_seg_lot);
			calculated_seg_topo->secondary_owner_seg_lot =  NULL;
			continue;
		}
		if (nvmeibt_disk_segment_is_owner_recovered(calculated_seg_topo) && is_owned_by_valid_owner) {
			// Switch to double owner
			// Keep the old owner_seg, (later in the code, add self as secondary owner)
			if (nvmeibt_praid_is_using_reset_registrants_for_owner_change(praid)) {
				// For now, no secondary owner. Will switch using RESET_REGISTRANTS
				NNVMEIBT_SEG_LOT_SET_OWNER(cvbn761, calculated_seg_lot, find_other_owner_in_praid_lot(calculated_praid_lot, i));
				calculated_seg_topo->secondary_owner_seg_lot =  NULL;
			}
			else {
				calculated_seg_topo->secondary_owner_seg_lot = calculated_seg_lot;	// Now we start the ownership transfer to self
			}
			continue;
		}
		// We are done with the various owners.
		// Now deal with the non-owners - DEAD / UNDER_RECOVERY_R/_I. No need for ownership transfer
		if (is_owned_by_valid_owner) {
			// Keep it as is
			calculated_seg_topo->secondary_owner_seg_lot =  NULL;
			continue;
		}
		// Not owner of any type. Does not have an owner
		// Only de_facto_owners can be selected, this simplifies the global working assumptions, although not really a must
		N_Tf(fyrhu33, "No prev valid owner seg=@UUID_8, to be calculated.", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
		NNVMEIBT_SEG_LOT_SET_OWNER(kiu98br, calculated_seg_lot, find_other_owner_in_praid_lot(calculated_praid_lot, i));
	}
	NFOUT;
}

static int compare_topo_for_clients(struct tTopoOfPraid *a, struct tTopoOfPraid *b)
{
	int i;

	if (a->header.n_segments != b->header.n_segments) {
		N_Tf(8wj2k9a, "n_segments: @INT!=@INT", a->header.n_segments, b->header.n_segments);
		return 1;
	}
	if (a->header.io_perms != b->header.io_perms) {
		N_Tf(xvtak239, "io_perms: @X!=@X", a->header.io_perms, b->header.io_perms);
		return 2;
	}
	if (a->header.blkset_sync_safety != b->header.blkset_sync_safety) {
		N_Tf(4caj301l, "blkset_sync_safety: @X!=@X", a->header.blkset_sync_safety, b->header.blkset_sync_safety);
		return 3;
	}
	for (i = 0; i < a->header.n_segments; i++) {
		if (a->s[i].access_mode != b->s[i].access_mode) {
			N_Tf(lxbqys5, "seg=@SEG access_mode: @STR!=@STR", a->s[i].uuid, nvmeibt_client_topo_seg_access_mode_to_str(a->s[i].access_mode), nvmeibt_client_topo_seg_access_mode_to_str(b->s[i].access_mode));
			return 4;
		}
		if (memcmp(a->s[i].uuid, b->s[i].uuid, sizeof(b->s[i].uuid)) != 0) {
			N_Tf(3tgq9je, "i=@INT UUID: @SEG!=@SEG", i, a->s[i].uuid, b->s[i].uuid);
			return 5;
		}
		if (memcmp(a->s[i].owners, b->s[i].owners, sizeof(b->s[i].owners)) != 0) {
			N_Tf(qc5a9j3l, "seg=@SEG owners arr differ", a->s[i].uuid);
			return 6;
		}
	}
	return 0;
}

static void leader_increase_version_and_set_clients_sync_cmd_due_to_changed_objects(struct nvmeibt_praid *praid, struct dirty_bits_state_stats *n, bool is_cold_recovery_done_now)
{
	bool									is_praid_topo_any_different;
	bool									is_delete = 0;
	bool									is_topo_for_clients_different;
	struct tTopoOfPraid						temp_topo_for_clients;
	int										compare_rv;
	union io_perms_bitfield					io_perms;
	bool									was_io_R_or_W_permitted;

	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;
	struct nvmeibt_praid_lot				*baseline_praid_lot = &praid_leader->baseline_praid_lot;
	struct nvmeibt_praid_topo_ctx			*baseline_praid_topo = &baseline_praid_lot->topo_ctx;
	struct nvmeibt_seg_lot					*calculated_seg_lot;

	NFIN;
	// Look for praid-level changes from prev_topology.
	is_praid_topo_any_different =
		(calculated_praid_topo->is_activated != baseline_praid_topo->is_activated) ||
		(calculated_praid_topo->leader_did_all_segs_sync_registrants != baseline_praid_topo->leader_did_all_segs_sync_registrants);
	N_Tf(iuj87sd, "praid=@UUID_LE registrants_sync_cmd=@REGISTRANTS_SYNC_CMD_STR(@PRAID_REGISTRANTS_SYNC_CMD_STR) is_activated=@IS_ACTIVATED(@IS_ACTIVATED) did_all_segs_sync_registrants=@DID_ALL_SEGS_SYNC_REGISTRANTS(@DID_ALL_SEGS_SYNC_REGISTRANTS)",
		nvmeibt_praid_UUID(praid),
		praid_registrants_sync_cmd_str(calculated_praid_topo->registrants_sync_cmd),
		praid_registrants_sync_cmd_str(baseline_praid_topo->registrants_sync_cmd),
		calculated_praid_topo->is_activated, baseline_praid_topo->is_activated,
		calculated_praid_topo->leader_did_all_segs_sync_registrants, baseline_praid_topo->leader_did_all_segs_sync_registrants);
	is_delete |= nvmeibt_praid_is_client_sync_cmd_delete(calculated_praid_topo->registrants_sync_cmd);

	// instead of comparing fields inside the segment topologies, build topo_for_clients and check if it differs from the current applied
	io_perms = nvmeibt_praid_calc_io_perms(praid, n->is_cold_recovery);
	memset(&temp_topo_for_clients, 0, sizeof(temp_topo_for_clients));
	nvmeibt_praid_lot_calc_topo_for_clients(calculated_praid_lot, &temp_topo_for_clients, io_perms.all, false);
	if (praid_leader->previous_topo_for_clients.header.n_segments == (int8_t)(-1)) {
		// We just became leader or it is a new praid
		if (praid->was_praid_ever_activated) {
			// The prev topo is the baseline_topo, build the corresponding clients topo.
			N_Tf(ik987t5, "new leader init prev clients topo praid=@UUID_LE", nvmeibt_praid_UUID(praid));
			io_perms = nvmeibt_praid_calc_io_perms(praid, nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(baseline_praid_lot->topo_ctx.registrants_sync_cmd));
			nvmeibt_praid_lot_calc_topo_for_clients(baseline_praid_lot, &praid_leader->previous_topo_for_clients, io_perms.all, false);
		}
		else {
			// There is no prev topo. No need to increase praid version
			N_Tf(iu87bq3, "new praid=@UUID_LE, reset prev clients topo", nvmeibt_praid_UUID(praid));
			praid_leader->previous_topo_for_clients = temp_topo_for_clients;
		}
	}
	compare_rv = compare_topo_for_clients(&temp_topo_for_clients, &praid_leader->previous_topo_for_clients);
	was_io_R_or_W_permitted = !!(praid_leader->previous_topo_for_clients.header.io_perms & 0x3);
	is_topo_for_clients_different = compare_rv;
	N_Tf(xbghqw34, "praid=@UUID_LE compare_rv=@INT", nvmeibt_praid_UUID(praid), compare_rv);
	if (compare_rv) {
		praid_leader->previous_topo_for_clients = temp_topo_for_clients;
	}

	// Look for segment-level changes
	XDLIST_FOREACH(calculated_seg_lot, &calculated_praid_lot->all_seg_lot_list) {
		struct nvmeibt_disk_segment_topo_ctx *calculated_seg_topo = &calculated_seg_lot->seg_topo;
		struct nvmeibt_disk_segment_topo_ctx *baseline_seg_topo = seg_lot_get_baseline_seg_topo(calculated_seg_lot);

		N_Tf(jki3439, "seg=@UUID_8 seg_ser_ver=@SEG_SER_VER(@ACTIVE_SEG_SER_VER)", nvmeibt_seg_lot_UUID_8(calculated_seg_lot), calculated_seg_topo->active_seg_ser_ver, baseline_seg_topo->active_seg_ser_ver);

		is_praid_topo_any_different |=
#if 1
			nvmeibt_disk_segment_are_topos_actionably_different(nvmeibt_seg_lot_UUID(calculated_seg_lot), calculated_seg_topo, baseline_seg_topo);
#else
			(calculated_seg_topo->dirty_bits_state != baseline_seg_topo->dirty_bits_state) ||
			(calculated_seg_topo->dirty_bits_init_mode != baseline_seg_topo->dirty_bits_init_mode) ||
			(calculated_seg_topo->stale_locks_init_mode != baseline_seg_topo->stale_locks_init_mode);
#endif

		is_delete |= nvmeibt_seg_lot_is_explicitly_deleted_in_config(calculated_seg_lot) ||
					 (nvmeibt_disk_segment_is_missing_in_config(calculated_seg_lot->my_seg) && !nvmeibt_disk_segment_is_x_done(calculated_seg_topo));
	}
	is_praid_topo_any_different |= is_topo_for_clients_different;
	praid_leader_calc_praid_version_and_registrants_sync_cmd(praid, n, is_delete, is_topo_for_clients_different, was_io_R_or_W_permitted, is_praid_topo_any_different, is_cold_recovery_done_now);

#if 1 // Nobody use these fields in the calculated topo ?
	// Update the seg_praid_version inside the segs
	XDLIST_FOREACH(calculated_seg_lot, &(calculated_praid_lot->all_seg_lot_list)) {
		calculated_seg_lot->seg_topo.seg_praid_version_major = calculated_praid_topo->praid_version_major;
		calculated_seg_lot->seg_topo.seg_praid_version_minor = calculated_praid_topo->praid_version_minor;
	}
#endif

	NVMEIBT_PRAID_TOPO_DUMP(sisolwv, nvmeibt_praid_UUID(praid), "Calculated", calculated_praid_topo);
	NFOUT;
}

static bool is_seg_zeroing_necessary(struct nvmeibt_seg_lot *seg_lot, struct nvmeibt_praid *praid)
{
	return (!nvmeibt_disk_is_explicitly_out_of_service(nvmeibt_seg_lot_get_disk(seg_lot)) &&
			!nvmeibt_disk_segment_is_mem_tbl_init_FIRST_USE_EVER(&seg_lot->seg_topo) &&
			praid->was_praid_ever_activated);
}

static int leader_update_deprecated_segments_state(struct nvmeibt_praid *praid, struct dirty_bits_state_stats *n)
{
	int										i;
	BOOL									is_any_updated = 0;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_seg_lot					*replacement_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;

	NFIN;
	// Called only from nvmeibt_praid_leader_calc_topo_main()
	// Deprecated seg-status:
	// 1. X (Delete the volume)
	// 2. R (Replace seg with another seg)
	// Disk-status
	// 1. If is_out_of_service, then no need to zero the seg whatsoever
	// If the disk is still in service then switch to replacement gradually:
	// 1. If an owner is deprecated, then switch to dual-owner
	// 2. If a non-owner is deprecated, or a dual-owner was synched then
	//    switch to X (or replace with a replacement seg)

	// The deprecated segments make the next move towards deprecation
	// "goto out" Since we deprecate only one of the praid's seg at a time for the following reasons
	// - Avoid too many simultaneous deprecations that can exceed the redundancy

	// The leader_switch_to_replacement_seg will be called after MGMT delete an old seg
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		if (!nvmeibt_seg_lot_is_deprecated_in_config(calculated_seg_lot)) {
			continue;
		}
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		// Found a deprecated seg in the topo_seg_lots[] array
		replacement_seg_lot = calculated_praid_lot->replacement_topo_seg_lots[i];
		if (nvmeibt_seg_lot_is_replaced_in_config(calculated_seg_lot)) {
			if (replacement_seg_lot) {
				if (nvmeibt_disk_segment_is_de_facto_owner(calculated_seg_topo) &&
					((n->non_crashed_de_facto_owners + calculated_praid_lot->from_config.redundancy) <= calculated_praid_lot->n_topo_seg_lots)) {
					N_Tf(hu882q3, "seg=@UUID_8 is deprecated but without it praid is not functional. Don't replace", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
					goto out;
				}
			} else {
				N_Ef(tvs63gi, "seg=@UUID_8 is deprecated but has no replacement", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
				nvmeibt_praid_mark_conf_corrupted(praid);
				goto out;
			}
		}

		N_Tf(26gashi, "seg=@UUID_8 deprecated=@DEPRECATED dirty_bits_state=@DIRTY_BITS_STATE_STR",
			nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
			calculated_seg_lot->from_config.deprecation_flag,
			dirty_bits_state_str(calculated_seg_topo->dirty_bits_state));
		if (!is_seg_zeroing_necessary(calculated_seg_lot, praid)) {
			// No need to zero, need to replace, and mark as X_DONE, so reported deprecated to mgmt
			// - The X_DONE is kind of overloaded, as it usually implies X_ZERO_DONE, but still OK for now
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(vfsue3j, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE);
			is_any_updated = 1;
			goto out;
		}
		// The disk is in service hence zeroing is needed
		if (nvmeibt_disk_segment_is_x_done(calculated_seg_topo)) {	// Finished zeroing
			goto out;
		}
		if (replacement_seg_lot) {
			if (nvmeibt_disk_segment_is_dirty_bits_state_calculated(replacement_seg_lot->seg_topo.dirty_bits_state)) {
				// Already started a deprecation I.e., the replacement already happened. Go for it.
				N_Wf(6gximwp, "seg=@UUID_8 is not X_DONE. Regardless, replacement already took effect, go for it", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
				goto out;
			}
		}
		// Deprecated in config. Did not reach the X_DONE yet, start the deprecation life-cycle
		is_any_updated = 1;
		NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(gd4hmvb, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO);
		nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
	}
out:
	NFOUT;
	return is_any_updated;
}

static void calc_dirty_bits_state_stats(struct nvmeibt_praid *praid, struct dirty_bits_state_stats *n)
{
	int										i;
	bool									is_new_dead;
	bool									was_any_owner;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_seg_lot					*owner_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*remote_seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*baseline_seg_topo;
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	calculated_dirty_bits_state;

//	FIN;
	memset(n, 0, sizeof(*n));
	n->are_all_owners_alive = 1;
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
		baseline_seg_topo = seg_lot_get_baseline_seg_topo(calculated_seg_lot);
		owner_seg_lot = calculated_seg_topo->owner_seg_lot;
		calculated_dirty_bits_state = calculated_seg_topo->dirty_bits_state;

		n->alive_stable += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE);
		n->alive_unstable += nvmeibt_disk_segment_is_alive_unstable(calculated_seg_topo);
		n->dead += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD);
		n->under_recovery_I += nvmeibt_disk_segment_is_under_recovery_I(calculated_seg_topo);
		n->under_recovery_R += nvmeibt_disk_segment_is_under_recovery_R(calculated_seg_topo);
		n->x += nvmeibt_disk_segment_is_x(calculated_seg_topo);
		n->launched_recoverers += nvmeibt_disk_segment_is_any_recoverer(baseline_seg_topo);
		n->owners_recoverer += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER);
		n->owners_recoverer_done += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE);
		n->owners_idle += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
		n->owners_recovered += !!(calculated_dirty_bits_state & NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED);
		// Do we have a de_facto_owner with all the locks and dirty bits that didn't crash
		n->remote_applied_de_facto_owners += !!nvmeibt_disk_segment_is_de_facto_owner(remote_seg_topo);
		n->are_all_owners_alive &= owner_seg_lot && nvmeibt_disk_segment_is_competent_owner(&owner_seg_lot->seg_topo);
		n->competent_owners += nvmeibt_disk_segment_is_competent_owner(calculated_seg_topo);
        n->de_facto_owners += nvmeibt_disk_segment_is_de_facto_owner(calculated_seg_topo);
		n->any_owner += nvmeibt_disk_segment_is_competent_owner(calculated_seg_topo);
		n->non_crashed_de_facto_owners += calculated_seg_topo->leader_seg_flags.has_ram_survived;
		n->recoverable_stable_de_facto_owners += calculated_seg_topo->leader_seg_flags.is_owner_ram_recoverable_stable;
		n->new_under_recov_I_or_R += nvmeibt_seg_topo_is_newly_added(calculated_seg_topo);
		was_any_owner = nvmeibt_disk_segment_is_competent_owner(baseline_seg_topo);
		n->is_it_possible_that_dirty_bits_were_set |= !was_any_owner;
		n->cold_recoverer += nvmeibt_disk_segment_is_ec_cold_recoverer(calculated_seg_topo);
		n->cold_recoverer_done += nvmeibt_disk_segment_is_ec_cold_recoverer_done(calculated_seg_topo);
		n->init_due_now += nvmeibt_disk_segment_leader_is_init_due_now(calculated_seg_lot);
	}
	XDLIST_FOREACH(calculated_seg_lot, &calculated_praid_lot->all_seg_lot_list) {
		baseline_seg_topo = seg_lot_get_baseline_seg_topo(calculated_seg_lot);
		is_new_dead = nvmeibt_disk_segment_leader_is_new_dead(calculated_seg_lot->my_seg);
		was_any_owner = nvmeibt_disk_segment_is_competent_owner(baseline_seg_topo);
		n->new_dead += is_new_dead;
		n->owner_died += (is_new_dead && was_any_owner);
		n->non_owner_died += (is_new_dead && !was_any_owner);
	}
	n->should_be_alive = calculated_praid_lot->n_topo_seg_lots - n->x;
	n->alive = n->should_be_alive - n->dead;
	n->are_all_segments_alive = (n->alive == n->should_be_alive);
	n->is_degraded = (n->alive < calculated_praid_lot->n_topo_seg_lots);
	n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty =
        (n->non_crashed_de_facto_owners < (calculated_praid_lot->n_topo_seg_lots - calculated_praid_lot->from_config.redundancy));
		// Should we add the condition "|| (!(praid->global_topo_as_is_ctx.is_activated))" // Back from non-activated is equivalent to post crash.
		// Since we might distribute non-activated topology on leader change (before it decided on this praid) etc.
		//  We now state that if a seg in APPEND_ENTRIES_REP is INIT_DONE (I.e. didn't crash),
		//  and the praid_version matches, then we can continue as if we never crashed with this
		//  seg. This should do the job.
	n->is_cold_recovery_init_needed = ((nvmeibt_praid_is_type_EC(praid) && praid->was_praid_ever_activated) &&
									   (n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty ||
										(n->cold_recoverer && n->under_recovery_I)));	// A seg went UNDER_RECOVERY_R --> UNDER_RECOVERY_I needs re-INIT
	n->is_cold_recovery = (n->is_cold_recovery_init_needed || nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(calculated_praid_lot->topo_ctx.registrants_sync_cmd));
	n->is_hot_recovery = (!n->is_cold_recovery && n->under_recovery_R);
//	FOUT;
}

#define NDUMP_DIRTY_BITS_STATE_STATS(name, n_segs, n)   																						\
	N_Tf(name,																	\
		 "n_segs=@N_SEGS alive_stable=@ALIVE_STABLE "							\
		 "alive_unstable=@ALIVE_UNSTABLE dead=@DEAD X_=@XXX "					\
		 "under_recovery_I=@UNDER_RECOVERY_I under_recovery=@UNDER_RECOVERY_R "	\
		 "owner_recoverer=@OWNER_RECOVERER "									\
		 "owner_recoverer_done=@OWNER_RECOVERER_DONE "							\
		 "owner_idle=@OWNER_IDLE owner_recovered=@OWNER_RECOVERED "				\
		 "new_under_recov_IR=@NEW_SEGMENTS any_owner=@ANY_OWNER "				\
		 "is_bringup_after_lose_mem=@IS_BRINGUP_AFTER_LOSE_MEM",				\
		 n_segs, n->alive_stable, n->alive_unstable,							\
		 n->dead, n->x, n->under_recovery_I, n->under_recovery_R,				\
		 n->owners_recoverer, n->owners_recoverer_done, n->owners_idle,			\
		 n->owners_recovered, n->new_under_recov_I_or_R, n->any_owner,			\
		 n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty)

#define NCALL_CALC_DIRTY_BITS_STATE_STATS(name, praid, n)	\
    calc_dirty_bits_state_stats(praid, n);					\
    NDUMP_DIRTY_BITS_STATE_STATS(name, praid->praid_leader.calculated_praid_lot.n_topo_seg_lots, (n))


static void convert_all_owner_recovered_to_owner_idle(struct nvmeibt_praid_lot *calculated_praid_lot)
{
	int								i;
	struct nvmeibt_seg_lot			*calculated_seg_lot;

	NFIN;
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		if (nvmeibt_disk_segment_is_owner_recovered(&calculated_seg_lot->seg_topo)) {
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(kiiomjy, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
		}
	}
	NFOUT;
}

static void __attribute__((unused)) convert_all_competent_owners_to_owner_idle_or_cold_recoverer(struct nvmeibt_praid_lot *praid_lot)
{
	int										i;
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_state_for_all_owners;

	NFIN;
	// We get here if we have new DEAD.
	// If we were in the middle of a cold recovery, then restart the cold recovery
	// Otherwise, start from OWNER_IDLE
	TODO("Possibly, we can convert all to OWNER_IDLE. Possibly, just leave them as is (as any kind of owner");
	dirty_state_for_all_owners = (nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(praid_lot->topo_ctx.registrants_sync_cmd) ?
								  NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER : NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		struct nvmeibt_seg_lot *seg_lot = praid_lot->topo_seg_lots[i];
		if (nvmeibt_disk_segment_is_competent_owner(&seg_lot->seg_topo)) {
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(t_zzz_2, seg_lot, dirty_state_for_all_owners);
		}
	}
	NFOUT;
}

static void decide_whether_to_activate_praid(struct nvmeibt_praid *praid, struct dirty_bits_state_stats *n)
{
	bool									is_in_working_condition;
	bool									is_waiting_for_all_segments;
	bool									is_X;
	struct timespec							cur_event_start_time = nvmeibt_global_get_cur_event_start_time();
	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;

	NFIN;
	if (!(praid_leader->first_activation_attempt_timespec.tv_sec)) {
		praid_leader->first_activation_attempt_timespec = cur_event_start_time;
	}
	praid_leader->is_waiting_for_timeout_since_activation_attempt &=
		((cur_event_start_time.tv_sec - praid_leader->first_activation_attempt_timespec.tv_sec) <
		 praid_time_from_activation_attempt_to_degraded_mode_sec);

	is_waiting_for_all_segments =  praid_leader->is_waiting_for_timeout_since_activation_attempt && !(n->are_all_segments_alive);
	is_X = (n->x == calculated_praid_lot->n_topo_seg_lots);
	N_Tf(c74hs9m, "PRAID_type=@PRAID_TYPE n_de_facto_owner_alive=@N_DE_FACTO_OWNER_ALIVE was_praid_ever_activated=@WAS_PRAID_EVER_ACTIVATED n_alive=@N_ALIVE ",
		 nvmeibt_praid_get_type_str(praid), n->de_facto_owners, praid->was_praid_ever_activated, n->alive);

	if (nvmeibt_praid_is_type_RAID1(praid)) {
		is_in_working_condition = n->de_facto_owners || (!(praid->was_praid_ever_activated) && (n->alive > 0));
	}
	else if (nvmeibt_praid_is_type_EC(praid)) {
		is_in_working_condition = ((n->de_facto_owners + calculated_praid_lot->from_config.redundancy >= calculated_praid_lot->n_topo_seg_lots) ||
								   (n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty &&
									(n->any_owner + calculated_praid_lot->from_config.redundancy >= calculated_praid_lot->n_topo_seg_lots)) ||
								   (!(praid->was_praid_ever_activated) &&
									(n->alive + calculated_praid_lot->from_config.redundancy >= calculated_praid_lot->n_topo_seg_lots)));
	}
	else if (nvmeibt_praid_is_jbod(praid)) {
		is_in_working_condition = n->de_facto_owners || (!(praid->was_praid_ever_activated) && (n->alive > 0));
	}
	else {
		N_Wf(fjsun6n, "unsupported PRAID_type=@PRAID_TYPE", nvmeibt_praid_get_type_str(praid));
		calculated_praid_topo->is_activated = 0;
		goto out;
	}

	// Do not start in degraded_mode too early. Wait for all-segment / timeout+(at_least_one_owner/is_first_use_ever...)
	calculated_praid_topo->is_activated = (is_X || (is_in_working_condition && !is_waiting_for_all_segments));
	if (calculated_praid_topo->is_activated) {
		praid_leader->is_waiting_for_timeout_since_activation_attempt = 0;
	}

	N_Tf(7vdwimd, "praid=@UUID_LE is_activated=@IS_ACTIVATED is_X=@IS_X is_in_working_condition=@IS_IN_WORKING_CONDITION is_waiting_for_all_segments=@IS_WAITING_FOR_ALL_SEGMENTS",
		nvmeibt_praid_UUID(praid), calculated_praid_topo->is_activated, is_X, is_in_working_condition, is_waiting_for_all_segments);
	// if not activated yet, due to wait for timeout then mark for recalc on the next cycle
	if (!calculated_praid_topo->is_activated) {
		if (praid_leader->is_waiting_for_timeout_since_activation_attempt) {
			// Trace args:[praid_id_str, tv_sec, praid_time_from_activation_attempt_to_degraded_mode_sec]
			N_Tf(s6hfinr, "praid=@UUID_LE Seconds_from_activation_attempt = @LLD < @LLD",
				nvmeibt_praid_UUID(praid),
				(int)(cur_event_start_time.tv_sec - praid_leader->first_activation_attempt_timespec.tv_sec),
				praid_time_from_activation_attempt_to_degraded_mode_sec);
		}
	}
out:
	NFOUT;
}

static bool leader_is_waiting_for_any_remote_seg_to_apply_topo(struct nvmeibt_praid *praid)
{
	struct nvmeibt_disk_segment				*disk_segment;
	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*remote_seg_topo;
	bool									is_waiting = 0;
	bool									is_remote_fully_synched_and_initialized;
	int										i;
	int										n_awaited_registrable = 0;
	int										n_awaited_non_registrable = 0;
	int										n_not_awaited_skipped = 0;
	struct timespec							now;
	int64_t									wait_time_nsec = 0;
	int64_t									max_nsec_wait_for_registrable_seg = 0;
	int64_t									max_nsec_wait_for_non_registrable_seg = 0;

	// This function has no effect on the logic itself, it can only skip the
	//  praid's topo calc if waiting for replies.

	NFIN;
	// First calculate the logical (state based) awaiting picture
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
		disk_segment = calculated_seg_lot->my_seg;
		is_remote_fully_synched_and_initialized = nvmeibt_seg_lot_is_remote_active_fully_synched(calculated_seg_topo, remote_seg_topo) &&
												  !nvmeibt_disk_segment_leader_is_remote_active_mem_tbl_init_command(remote_seg_topo);
		if (	(disk_segment->seg_leader.is_removed_from_remote_applied ||	// Do not wait if explicitly removed
				 disk_segment->seg_leader.baseline_seg_lot.is_replacement ||	// the seg was not a part of topo, nothing to wait for
				 !nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo))) {
			n_not_awaited_skipped++;
			N_Tf(cvs75nw, "Skipping seg=@UUID_8 @DIRTY_BITS_STATE_STR praid_version remote=@PRAID_VERSION,@PRAID_VERSION (leader=@PRAID_VERSION,@PRAID_VERSION) "
						  "is_synched=@BOOL is_registrants_synchronizer=@BOOL are_praid_registrants_aligned_with_sync_cmd=@BOOL",
				nvmeibt_seg_lot_UUID_8(calculated_seg_lot), dirty_bits_state_str(calculated_seg_topo->dirty_bits_state),
				remote_seg_topo->seg_praid_version_major, remote_seg_topo->seg_praid_version_minor,
				calculated_seg_topo->seg_praid_version_major, calculated_seg_topo->seg_praid_version_minor,
				nvmeibt_seg_lot_is_remote_active_fully_synched(calculated_seg_topo, remote_seg_topo),
				calculated_seg_topo->is_registrants_synchronizer,
				remote_seg_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd);
			continue;
		}
		if (	(is_remote_fully_synched_and_initialized ||		//Topo wise, the applied values are relevant
				 (!(praid->was_praid_ever_activated) && nvmeibt_disk_segment_leader_is_state_progressible(remote_seg_topo)))) { // ALIVE first time ever
			N_Tf(tvauyrw, "seg=@UUID_8 is good to go", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
			continue;
		}
		// An awaited seg
		N_Tf(rovxw2m, "Awaiting seg=@UUID_8 @DIRTY_BITS_STATE_STR praid_version remote=@PRAID_VERSION,@PRAID_VERSION (leader=@PRAID_VERSION,@PRAID_VERSION) "
					  "is_synched=@BOOL is_registrants_synchronizer=@BOOL are_praid_registrants_aligned_with_sync_cmd=@BOOL",
			nvmeibt_seg_lot_UUID_8(calculated_seg_lot), dirty_bits_state_str(calculated_seg_topo->dirty_bits_state),
			remote_seg_topo->seg_praid_version_major, remote_seg_topo->seg_praid_version_minor,
			calculated_seg_topo->seg_praid_version_major, calculated_seg_topo->seg_praid_version_minor,
			nvmeibt_seg_lot_is_remote_active_fully_synched(calculated_seg_topo, remote_seg_topo),
			calculated_seg_topo->is_registrants_synchronizer,
			remote_seg_topo->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd);
		if (nvmeibt_disk_segment_is_dirty_bits_state_registrable(calculated_seg_topo->dirty_bits_state)) {
			n_awaited_registrable++;	// Important to the topo (R/W & W)
		} else {
			n_awaited_non_registrable++;	// DEAD / _Init
		}
		if (nvmeibt_disk_segment_is_carrying_advanced_praid_version(calculated_praid_topo, remote_seg_topo)) {
			N_Tf(t_cc_0, "The seg=@UUID_8 is carrying advanced praid version! Send to it a full config&topo", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
		}
	}
	//
	getnstimeofday_boot(&now);
	wait_time_nsec = timespec_diff_ns(now, praid_leader->last_serialization_timestamp);
	max_nsec_wait_for_registrable_seg = nvmeibt_raft_get_praid_leader_max_nsec_wait_for_registrable_seg_to_apply(praid_leader->did_any_client_report_about_problems);
	max_nsec_wait_for_non_registrable_seg = nvmeibt_raft_get_praid_leader_max_nsec_wait_for_non_registrable_seg_to_apply(praid_leader->did_any_client_report_about_problems);
	//
	if (n_awaited_registrable + n_awaited_non_registrable + n_not_awaited_skipped > calculated_praid_lot->from_config.redundancy) {
		if (wait_time_nsec < max_nsec_wait_for_registrable_seg) {
			is_waiting = 1;
		} else {
			N_Tf(ctehw78, "Giving up on praid, will end-up as is_activated=0 in order to report the issue to mgmt");
			n_awaited_registrable = 0;
			n_awaited_non_registrable = 0;
		}
		goto out;
	}
	// Now that we have the logical picture, see if we decide not to wait due to timeout considerations
	if (n_awaited_registrable || n_awaited_non_registrable) {
		if (n_awaited_registrable) {
			// The registrable segs are essential for working with this topo. If we wait for one then the non-reg are less important
			if (wait_time_nsec < max_nsec_wait_for_registrable_seg) {
				is_waiting = 1;
			} else {
				N_Tf(5bdhd73, "Giving up on non responsive registrable segments");
				n_awaited_registrable = 0;
			}
		}
		if (n_awaited_non_registrable) {
			if (wait_time_nsec < max_nsec_wait_for_non_registrable_seg) {
				is_waiting = 1;
			} else {
				N_Tf(hdjm2x9, "Giving up on non responsive non_registrable segments");
				n_awaited_non_registrable = 0;
			}
		}
	}
out:
	praid_leader->is_waiting_for_any_remote_seg_to_apply_topo = is_waiting;
	if (is_waiting) {
		N_Tf(6bdhdiw, "Awaiting. praid=@UUID_LE nsec=@LLD(max registrable=@LLD non_registrable=@LLD), "
					  "n_awaited(registrable=@INT + non_registrable=@INT) + n_not_awaited_skipped=@INT >? redundancy=@INT",
			 nvmeibt_praid_UUID(praid), wait_time_nsec, max_nsec_wait_for_registrable_seg, max_nsec_wait_for_non_registrable_seg,
			 n_awaited_registrable, n_awaited_non_registrable, n_not_awaited_skipped, calculated_praid_lot->from_config.redundancy);
	}
	else {
		praid_leader->did_any_client_report_about_problems = 0;	// Not waiting. Reset before the next topo change, and wait for peer-TOMAs to report
	}

	NFOUT;
	return is_waiting;
}

static void leader_calc_all_segs_init_modes(struct nvmeibt_praid *praid, struct dirty_bits_state_stats *n)
{
	int										i;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid->praid_leader.calculated_praid_lot;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	bool									is_journaled = nvmeibt_praid_is_type_EC(praid);
	// A word about INIT_DONE
	// - It does not survive COLD startup after crash
	// - A non-owner seg, requires init
	// - If we have a new seg (HOT), then the live owners are required to TURN_ON all dirty
	NFIN;
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		if (nvmeibt_seg_lot_is_X_in_config(calculated_seg_lot) || nvmeibt_seg_topo_is_x(calculated_seg_topo)) {
			nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
			continue;
		}
		if (calculated_seg_lot->is_FIRST_USE_EVER) {
			NTOMA_ASSERT(27amos0, nvmeibt_disk_segment_is_mem_tbl_init_FIRST_USE_EVER(calculated_seg_topo), "dirty_init_mode=@STR", mem_tbl_init_mode_str(calculated_seg_topo->dirty_bits_init_mode));
			nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER);
			continue;
		}
		if (!nvmeibt_praid_is_segments_dirty_bit_relevant(praid) ) {
			nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT);
			continue;
		}
		if (!nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo)) {
			N_Tf(bydwjsi, "Skipping seg=@UUID_8 @DIRTY_BITS_STATE_STR",
				 nvmeibt_seg_lot_UUID_8(calculated_seg_lot), dirty_bits_state_str(calculated_seg_topo->dirty_bits_state));
			continue;
		}
		if (n->is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty) {	// COLD
			/*
			 * Some owners went down and lost their memory tables. Some might have
			 * survived it. We will init only those that lost it, and if ALIVE_STABLE
			 * (clean shutdown with the latest praid_version) we will init them from persist.
			 * This will save some work as opposed to setting all memory as suspect
			 * Note that in EC the stale_locks are always seto to TURN_ALL_OFF,
			 * because cold recovery will detect and recover all the in-flight
			 * (stale) i/o write transactions using the journal entries.
			 */

			if (nvmeibt_disk_segment_is_competent_owner(calculated_seg_topo)) {
				if (is_journaled) {
					NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(bchs9nu,	calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF);
				}
				if (calculated_seg_topo->leader_seg_flags.has_ram_survived) {
					// Leave as is, do not turn on unnecessary bits. It will require extra work
				} else if (calculated_seg_topo->leader_seg_flags.is_owner_ram_recoverable_stable) {	// FROM_PERSIST
					if (!is_journaled) {
						NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(7dbsh23, calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST);
					}
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE(1vsjytg,	calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST);
				} else {	// TURN_ALL_ON
					if (!is_journaled) {
						NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(6cvsjhs, calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON);
					}
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE(1v4fhag,	calculated_seg_lot ,(n->is_it_possible_that_dirty_bits_were_set ?
																										 NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON :
																										 NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF));
				}
			}
			else {
				nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF);
			}
		}
		else {	// HOT - I.e., we trust the memory tables of the owners, and all new under_recovery_I are reset
			if (nvmeibt_disk_segment_is_under_recovery_I(calculated_seg_topo)) {
				TODO(Consider using this logic all over. First identify a revived disk. Only they require init, other than if a new seg was introduced);
				NNVMEIBT_SEG_LOT_SET_STALE_LOCKS_INIT_MODE(6g7sg37,	calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF);
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS_INIT_MODE(1vs8mag,	calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF);
			}
		}
	}
	// The following was commented out. Added by Ronen to avoid heart-attacks
	// commit 155361efbdfa0b68786b0cc93544afecf89658a8
	// Author: DanielHsH
	//	Toma: Disable dconvict turn on for R1. Client does that
    // Change-Id: I7353a60ca0138fe3346502362249cfe7718e1a5f
	//	if (n->new_under_recov_IR && nvmeibt_disk_segment_is_de_facto_owner(seg_leader_topo)) { // The owner was alive and kicking, but we have new segments, and need to turn d-convict on th>
	//  	if (!is_journaled) {    TODO(remove this condition for R1 as well. Clnt turns on convicts)
	//			NNVMEIBT_DISK_SEGMENT_SET_DIRTY_BITS_INIT_MODE(trace_17_praid_leader_calc_all_segs_init_modes, disk_segment, seg_leader_topo, NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON);
	//		}
	//	}
	NFOUT;
}

/*
 * The core of the topology
 * This function is called after:
 * 1. (Maybe not all) the peers reported about their local disk_segments
 * 2. We have a baseline_praid_lot to start from
 * The logic goes like this:
 * 1. Start from the calculated_praid_lot that was just duplicated from baseline_praid_lot
 * 2. Update, where the remote seg is more up-to-date
 */

int nvmeibt_praid_leader_calc_topo_main(struct nvmeibt_praid *praid)
{
	int										rv = -1;
	int										i;
	// Counters for the global_topology values
	struct dirty_bits_state_stats			n;

	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot				*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_praid_topo_ctx			*calculated_praid_topo = &calculated_praid_lot->topo_ctx;
	struct nvmeibt_seg_lot					*calculated_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*calculated_seg_topo;
	struct nvmeibt_disk_segment_topo_ctx	*remote_seg_topo;
	bool									is_cold_recovery_done_now = 0;	// Not in "n.", since states change (RECOVERER_DONE-->OWNER_IDLE) and recalc

	NFIN;
	N_Tf(u7ey3bk, "Working on praid=@UUID_LE", nvmeibt_praid_UUID(praid));
	praid_leader->is_waiting_for_any_remote_seg_to_apply_topo = 0;
	if (nvmeibt_praid_is_conf_corrupted(praid)) {
		N_Tf(vdts7hw, "praid=@UUID_LE conf_corrupted. Skipping", nvmeibt_praid_UUID(praid));
		goto out_not_activated;
	}

	// Deprecation state
	if (nvmeibt_praid_is_deprecated_in_config(praid)) {
		nvmeibt_praid_lot_upd_registrants_sync_cmd(calculated_praid_lot, PRAID_REGISTRANTS_SYNC_CMD_DELETE);
		XDLIST_FOREACH(calculated_seg_lot, &calculated_praid_lot->all_seg_lot_list) {
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(d64fsho, calculated_seg_lot,
											((nvmeibt_disk_segment_is_x_done(remote_seg_topo) ||
											  nvmeibt_disk_segment_is_x_done(calculated_seg_topo) ||
											  !is_seg_zeroing_necessary(calculated_seg_lot, praid)) ?
											 NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE : NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO));
			//nvmeibt_seg_lot_set_all_init_modes(calculated_seg_lot, NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT); init modes will be updated ater goto
		}
		calculated_praid_topo->is_activated = 1;
		goto calc_owners;
	}
	if (nvmeibt_praid_is_being_deleted(praid)) {
		goto out_not_activated;
	}

	// First-time ever raid (not found in toma persistency)
	if (!(praid->was_praid_ever_activated)) {
		nvmeibt_global_adaptive_timeouts()->n_praids.new_in_topo++;
		N_Tf(qmzind8, "First-time PRAID. praid_id=@UUID_LE", nvmeibt_praid_UUID(praid));
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(7feiywl, calculated_seg_lot,
													  (nvmeibt_seg_lot_is_deprecated_in_config(calculated_seg_lot)) ?
													   NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE : NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
			NNVMEIBT_SEG_LOT_SET_OWNER(o40skis, calculated_seg_lot, NULL);	// Will be calculated later on
		}
		calculated_praid_topo->is_activated = 1;
		goto calc_owners;
	}
	else {
		N_Tf(6viwjs, "Not a first-time PRAID");
	}

	// Remember, we just copied the baseline_praid_lot into calculated_praid_lot completely from scratch
	if (leader_is_waiting_for_any_remote_seg_to_apply_topo(praid)) {
		N_Tf(6hmziem, "praid=@UUID_LE awaiting some remote segs. Skipping", nvmeibt_praid_UUID(praid));
		goto out;
	}
	//  No leftovers from previous call to this function that failed to generate an is_activated praid.
	XDLIST_FOREACH(calculated_seg_lot, &calculated_praid_lot->all_seg_lot_list) {
		nvmeibt_disk_segment_leader_sync_with_remote_applied(calculated_seg_lot);
		nvmeibt_seg_lot_leader_convert_unusable_to_dead(calculated_seg_lot);
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		remote_seg_topo = seg_lot_get_remote_seg_topo(calculated_seg_lot);
		calculated_seg_topo->leader_seg_flags.has_ram_survived =
			(nvmeibt_disk_segment_is_de_facto_owner(calculated_seg_topo) &&
             nvmeibt_disk_segment_is_de_facto_owner(remote_seg_topo) &&
			 nvmeibt_disk_segment_is_mem_tbl_init_done_fully(calculated_seg_topo));
		calculated_seg_topo->leader_seg_flags.is_owner_ram_recoverable_stable =
			(nvmeibt_disk_segment_is_competent_owner(calculated_seg_topo) &&
             nvmeibt_disk_segment_is_alive_stable(remote_seg_topo));
		// If the init_mode from remote_applied is past FIRST_USE_EVER, then erase calculated_seg_lot->is_FIRST_USE_EVER
		calculated_seg_lot->is_FIRST_USE_EVER = (calculated_seg_lot->is_FIRST_USE_EVER && !nvmeibt_disk_segment_is_mem_tbl_init_beyond_FIRST_USE_EVER(remote_seg_topo));
	}
	NCALL_CALC_DIRTY_BITS_STATE_STATS(7cb4hkm, praid, &n);
	// A rebuild that somehow lost it will restart from scratch (Defensive code)
	if (n.under_recovery_R &&
		!nvmeibt_praid_is_client_sync_cmd_switch_topo_under_recovery(calculated_praid_topo->registrants_sync_cmd)) {
		// convert the UNDER_RECOVERY_R segs to UNDER_RECOVERY_I. This will fix it.
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			if (nvmeibt_disk_segment_is_under_recovery_R(calculated_seg_topo)) {	// Convert UNDER_RECOVERY_R --> UNDER_RECOVERY_I
				enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE new_dbits_state;
				new_dbits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I;
				if (	(nvmeibt_praid_is_client_sync_cmd_reset_registrants(calculated_praid_topo->registrants_sync_cmd) ||
						 nvmeibt_praid_is_client_sync_cmd_switch_topo_X(calculated_praid_topo->registrants_sync_cmd))) {
					N_Tf(6scbyiw, "RESET_REGISTRANTS after COLD rebuild or another seg died seg=@UUID_8 @DIRTY_BITS_STATE_STR-->@DIRTY_BITS_STATE_STR",
						 nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
						 dirty_bits_state_str(calculated_seg_topo->dirty_bits_state),
						 dirty_bits_state_str(new_dbits_state));
				} else {
					N_Wf(tcv83h2, "Surprise _R->_I seg=@UUID_8 @DIRTY_BITS_STATE_STR-->@DIRTY_BITS_STATE_STR",
						 nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
						 dirty_bits_state_str(calculated_seg_topo->dirty_bits_state),
						 dirty_bits_state_str(new_dbits_state));
				}
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(vedwtw7, calculated_seg_lot, new_dbits_state);
			}
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(4hdyhkm, praid, &n);
	}
/* -- Commented out by Ronen on Nov03 2020.
	-- For the case of n.new_dead==true. The criteria was any_dead and this affected only the owners
	-- if (n.is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty)
    	-- We repeat the same decision based on the same criteria
    -- else (when a cold_recoverer dies in the middle of cold_recovery)
    	-- sync_with_remote_applied() will mark it as DEAD, and we have a new topo for cold_recovery
    	-- The cold-recovery "if" will set all of them to EC_COLD_RECOVERER (even if done)
    -- else (the next decision point is at the end of cold_recovery, when all are recoverer_done)
		-- The condition "if (n.are_all_owners_alive && (n.cold_recoverer == 0) && (n.cold_recoverer_done > 0)) {"
		--  is better off without the removed code, a recoverer_done is valid for the surviving segs
    -- else		// I.e., in the midst of life (HOT)
	    -- The HOT code is unrelated to COLD_RECOVERER, hence not affected by this commenting out of code
		-- If an owner died then we skip the "if (n.are_all_owners_alive &&"

	if (n.new_dead) {
		// Forget about the running rebuilds, no recoveres continuation
		convert_all_competent_owners_to_owner_idle_or_cold_recoverer(calculated_praid_lot);
		NCALL_CALC_DIRTY_BITS_STATE_STATS(vd6bsjow, praid, &n);
	}
*/
	praid_leader_check_whether_all_segments_registrants_are_aligned(praid);
	// Now we want to apply some global logic. First count them.
	// Deprecation
	if (leader_update_deprecated_segments_state(praid, &n)) {
		// Updated - Lost the counting of deprecating, X, and replacement segs
		NCALL_CALC_DIRTY_BITS_STATE_STATS(vd6b4ib, praid, &n);
	}
	// Activation
	decide_whether_to_activate_praid(praid, &n);
	if (!calculated_praid_topo->is_activated) {
		N_Tf(hbf6sek, "Going out, not activated");
		goto out_not_activated;
	}
	is_cold_recovery_done_now = (n.cold_recoverer_done && (n.cold_recoverer_done == n.launched_recoverers));
	// The applied state is ALIVE_* only if the node was just brought up
	// Reminder: The old_state of the praid's segments was owner*/under_recovery/dead
	// For EC all owners need to recover their journals, hence become EC_COLD_RECOVERE
	if (n.is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty) {
		enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	dirty_state_for_all_owners;

		// non-EC OWNER_IDLE will become OWNER_RECOVERER later on (if we have a hot recovery)
		dirty_state_for_all_owners = (nvmeibt_praid_is_type_EC(praid) ? NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER : NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
		//
		N_Tf(7dhwimg, "bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty "
					  "competent_owners=@INT non_crashed_de_facto_owners=@INT recoverable_stable_de_facto_owners=@INT",
			 n.competent_owners, n.non_crashed_de_facto_owners, n.recoverable_stable_de_facto_owners);
		// Note that if a node went down before the others, then its segments' state was DEAD
		praid_lot_reset_registrants_sync_status(calculated_praid_lot);
		// Remember: after crash. For now, assign owners as EC_COLD_RECOVERES or OWNER_IDLE
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			// If X_<DONE/ZERO>, then outside the dirty life-cycle - continue
			if (nvmeibt_disk_segment_is_x(calculated_seg_topo)) {
				N_Tf(crt2biz, "@STR - leave as is", dirty_bits_state_str(calculated_seg_topo->dirty_bits_state));
				continue;
			}
			// DEAD segments
			if (!nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo)) {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(dg6sjao, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD);	// Probably repeated later on
				continue;
			}
			// Owners
			if (nvmeibt_disk_segment_is_competent_owner(calculated_seg_topo)) {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(dg62jao, calculated_seg_lot, dirty_state_for_all_owners);
				continue;
			}
			// Non owners EC-or-not
			// EC cold_recovery ignores the non-owners. We prepare the UNDER_RECOVERY for the hot recovery that will follow
			// This code is not really relevant for RAID1 since the hot recovery logic will reassign the UNDER_RECOVERY
			NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(t3csjao, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I);
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(vaim5nq, praid, &n);
		goto calc_recoveres_and_under_recovery;
	}
	// Not is_bringup_of_a_familiar_praid_after_losing_mem_stale_and_dirty (Not COLD, we stay HOT)
	// We have enough surviving leaders, and any seg that went down (even ALIVE_STABLE), will go UNDER_RECOVERY

	// Unused "if" in this version (always false, doesn't have any effect). OWNER_RECOVERED is impossible.
	if (!nvmeibt_praid_is_using_reset_registrants_for_owner_change(praid)) {
		// If we have a recovered seg. and done clients-sync (of a TOPO_SWITCH_D), go to the next stage
		if ((n.owners_recovered > 0) && calculated_praid_topo->leader_did_all_segs_sync_registrants) {
			convert_all_owner_recovered_to_owner_idle(calculated_praid_lot);
			NCALL_CALC_DIRTY_BITS_STATE_STATS(3vha9mh, praid, &n);
		}
	}
	if (n.cold_recoverer_done) {
		// Are we done COLD_RECOVERY
		if (is_cold_recovery_done_now) {
			N_Tf(sd9k3ld, "Finished cold recovery");
			// Now run HOT dirty-rebuild as needed
			// All previously owner segs (recoverer_done) are in sync.
			// Move them to OWNER_IDLE If there is a need for HOT-recovery, then the _IDLE will switch to OWNER_RECOVERER later
			// The UNDER_RECOVERY_R remain as is
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
				if (nvmeibt_disk_segment_is_ec_cold_recoverer_done(&calculated_seg_lot->seg_topo)) {
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(vc73hsu, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
				} else {
					// Non Owners (do not touch them)
					// They do not take part in cold recovery. Will be recovered in an upcoming hot recovery, a few "if"s below
				}
			}
		} else {
			// Do not change the leader's serialized topo by interim COLD_RECOVERER-->COLD_RECOVERER_DONE
			// Also takes care of restarting a recovery in case one of the owners died
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
				if (nvmeibt_disk_segment_is_ec_cold_recoverer_done(&calculated_seg_lot->seg_topo)) {
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(b9dk3os, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER);
				}
			}
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(r54ecd2, praid, &n);	// Recalc since too delicate to follow the effect on all stats
	}
	if (n.owners_recoverer_done) {
		// If done with a HOT recovery cycle, then:
		// all the old owners become OWNER_IDLE and the UNDER_RECOVERY_R also become OWNER_IDLE (OWNER_RECOVERED had we not used "reset_registrants")
		// HOT: If there is a need for recovery, then the _IDLE will switch to OWNER_RECOVERER later
		if (n.owners_recoverer_done == n.launched_recoverers) {
			N_Tf(trace_25_praid_nvmeibt_praid_leader_calc_segments_states_and_owners, "Finished recovery");
			// All previously active are in sync. Move them to OWNER (incl. the UNDER_RECOVERY_R)
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
				calculated_seg_topo = &calculated_seg_lot->seg_topo;
				if (nvmeibt_disk_segment_is_under_recovery_R(calculated_seg_topo) &&
					nvmeibt_disk_segment_is_under_recovery_R(seg_lot_get_baseline_seg_topo(calculated_seg_lot))) {	// I.e., It was recovered as part of the last recovery
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(6bjd7va, calculated_seg_lot,
															  (nvmeibt_praid_is_using_reset_registrants_for_owner_change(praid) ?
															   NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE : NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED));
					calculated_seg_topo->leader_seg_flags.is_newly_added_seg = 0;
				}
				else if (nvmeibt_disk_segment_is_de_facto_owner(calculated_seg_topo)) {
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(tcvsome, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE);
				}
			}
		} else {
			// Do not change the leader's serialized topo by interim OWNER_RECOVERER-->OWNER_RECOVERER_DONE
			// Also takes care of restarting a recovery in case one of the owners died
			for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
				calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
				if (nvmeibt_disk_segment_is_owner_recoverer_done(&calculated_seg_lot->seg_topo)) {
					NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(3vsbkr6, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER);
				}
			}
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(u7y6tbf, praid, &n);	// Recalc since too delicate to follow the effect on all stats
	}
	// We decided not to go cold
	// The following handles network disconnect + a few owners reboot (all at once)
	// If enough OWNERS survived (with valid memory maps of dirty&stale), and we have
	//  an owner that went remote_ALIVE_any, then we need to DIRTY_REBUILD it, probably
	//  with 0 dirty_bits, in order to recover its stale locks
	if (n.de_facto_owners > n.non_crashed_de_facto_owners) {	// We have an owner remote ALIVE
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			if (!nvmeibt_disk_segment_is_de_facto_owner(&calculated_seg_lot->seg_topo)) {
				continue;
			}
			if (nvmeibt_disk_segment_is_alive_unstable(seg_lot_get_remote_seg_topo(calculated_seg_lot)) ||
				nvmeibt_disk_segment_is_alive_stable(seg_lot_get_remote_seg_topo(calculated_seg_lot))) {
				// We decided not to go cold, so we continue with the I/O ASAP without it, and recover this seg
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(5wmisej, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I);
			}
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(i98kmj0, praid, &n);	// Recalc since too delicate to follow the effect on all stats
	}
calc_recoveres_and_under_recovery:
	// All newly functioning segments are marked as [NEW_]UNDER_RECOVERY_I
	if (n.alive_unstable || n.new_under_recov_I_or_R) {
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			calculated_seg_topo = &calculated_seg_lot->seg_topo;
			if (nvmeibt_disk_segment_is_alive_stable(calculated_seg_topo)) {
				N_Ef(bshw4nv, "Unexpected ALIVE_STABLE id=@UUID_8", nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
				goto out;
			}
			if (nvmeibt_disk_segment_is_alive_unstable(calculated_seg_topo)) {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(bsopli3, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I);
			}
		}
		NCALL_CALC_DIRTY_BITS_STATE_STATS(okqwe98, praid, &n);	// Recalc since too delicate to follow the effect on all stats
	}
	// Do we need to run HOT recovery
	if (n.is_hot_recovery) {
		// Mark all the de_facto_owners as recoverer
		for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
			calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
			// Owners are marked as recoverer
			// This does not includes those that are owner_recovered (but we don't have OWNER_RECOVERED in the current version).
			// Recoverer_done from previous praid_version will restart recovery, but it will have no dirty_bits to work on (unless we have a new seg)
			if (nvmeibt_disk_segment_is_de_facto_owner(&calculated_seg_lot->seg_topo)) {
				NNVMEIBT_SEG_LOT_SET_DIRTY_BITS(bdhsy3a, calculated_seg_lot, NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER);
			}
		}
	}

calc_owners:
	NCALL_CALC_DIRTY_BITS_STATE_STATS(fr123zc, praid, &n);	// Recalc since too delicate to follow the effect on all stats
	leader_calc_all_segs_init_modes(praid, &n);
	calc_owners_per_disk_segment(praid);
	if (!calculated_praid_topo->is_activated) {
		N_Tf(udh2nch, "Going out, not activated");
		goto out_not_activated;
	}
	leader_increase_version_and_set_clients_sync_cmd_due_to_changed_objects(praid, &n, is_cold_recovery_done_now);

// out_ok:
	for (i = 0; i < calculated_praid_lot->n_topo_seg_lots; i++) {
		calculated_seg_lot = calculated_praid_lot->topo_seg_lots[i];
		calculated_seg_topo = &calculated_seg_lot->seg_topo;
		if (!nvmeibt_disk_segment_is_dirty_bits_state_calculated(calculated_seg_topo->dirty_bits_state)) {
			N_Wf(hyy7652, "seg=@UUID_8 dirty_bits_state=@DIRTY_BITS_STATE (It's OK if a seg is new)",
				 nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
				 dirty_bits_state_str(calculated_seg_topo->dirty_bits_state));
		}
		if (	nvmeibt_disk_segment_leader_is_state_progressible(calculated_seg_topo) &&
				(calculated_seg_topo->dirty_bits_init_mode ==  NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED ||
				 calculated_seg_topo->stale_locks_init_mode == NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED)) {
			N_Ef(6fvdg31, "seg=@UUID_8 init_mode=(@MEM_CTL_INIT_MODE_STR,@MEM_CTL_INIT_MODE_STR)",
				nvmeibt_seg_lot_UUID_8(calculated_seg_lot),
				mem_tbl_init_mode_str(calculated_seg_topo->dirty_bits_init_mode),
				mem_tbl_init_mode_str(calculated_seg_topo->stale_locks_init_mode));
		}
	}
	rv = 0;
	nvmeibt_global_adaptive_timeouts()->n_praids.with_new_dead += (n.new_dead > 0);
	goto out;
out_not_activated:
	N_Tf(xm985u4, "praid->is_activated=0");
	if (praid_leader->baseline_praid_lot.topo_ctx.is_activated) {
		nvmeibt_global_adaptive_timeouts()->n_praids.became_not_activated += 1;
		//praid_leader->baseline_praid_lot.topo_ctx.is_activated = 0;
		nvmeibt_praid_lot_duplicate_content(&praid_leader->calculated_praid_lot, &praid_leader->baseline_praid_lot);
		calculated_praid_topo->is_activated = 0;

		++(calculated_praid_topo->praid_version_minor);	// Increase praid_version_minor, allow distribution
		calculated_praid_topo->topo_idx_updated = leader_get_next_topology_version();
		XDLIST_FOREACH(calculated_seg_lot, &(calculated_praid_lot->all_seg_lot_list)) {
			calculated_seg_lot->seg_topo.seg_praid_version_minor = calculated_praid_topo->praid_version_minor;
		}
		SET_RAFT_LEADER_NEXT_TOPOLOGY_VERSION(c6sbv2i);
	}
	else {
		// Do not increase version, as it will mean that we lost track
	}
	rv = 0;
out:
	// is_recalc_required if waiting for time to ellapse
	// Otherwise, something must happen (such as remote_applied change) to trigger it
	if (praid_leader->is_waiting_for_timeout_since_activation_attempt || praid_leader->is_waiting_for_any_remote_seg_to_apply_topo) {
		NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(koqwn87, praid);
	}
	else {
		if (calculated_praid_topo->is_activated) {
			validate_owners(praid);
			praid->was_praid_ever_activated = 1;
		}
	}
	NFOUT;
	return rv;
}

void nvmeibt_praid_dump(__attribute__((__unused__)) const struct nvmeibt_praid *praid)
{
#ifdef TOMA_DEBUG
	const struct nvmeibt_praid_config *f = &praid->from_config;
	N_Tf(trace_praid_nvmeibt_praid_dump, "Config data: uuid=@UUID_LE @C_VOL_VER type=@TYPE_STR chunk=@UUID_LE stripe_idx=@STRIPE_IDX",
		 nvmeibt_praid_UUID(praid), f->version, nvmeibt_praid_type_str(praid->praid_mgmt.type), &(praid->praid_mgmt.chunk_id), praid->praid_mgmt.stripe_idx);
#endif
}

#define NFREE_PRAID(name, __praid)																	\
({																									\
	if (__praid) {																					\
		XDLIST_DEL(&(__praid->global_report_to_mgmt_praid_link));									\
		XDLIST_DEL(&(__praid->praid_topo_recalc_link));												\
		NNVMEIBT_BUF_FREE(name ## _1, &(__praid)->praid_leader.segs_wire_topo_buf);					\
		NNVMEIBT_BUF_FREE(name ## _sc, &(__praid)->praid_leader.topo_config_array_of_its_serialized_segs_conf); \
		NNVMEIBT_BUF_FREE(name ## _wc, &(__praid)->praid_leader.topo_config_praid_and_segs_wire_conf_buf); \
		NNVMEIBT_TOMA_FREE(name ## _2, __praid);													\
		(__praid) = NULL;																			\
	}																								\
})

int nvmeibt_praid_remove(struct nvmeibt_praid *praid) {
	int rv = -1;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();
	NFIN;
	if (praid) {
		N_Tf(jiu87yt, "Removing praid=@UUID_LE", nvmeibt_praid_UUID(praid));
		nvmeibt_topology_leader_mark_recalc_required();
		SET_RAFT_LEADER_NEXT_TOPOLOGY_VERSION(cybajh2);
		NNVMEIBT_HASH_DEL_OBJ_new(bhuy763, cur_topo->praids_hash_by_uuid, praid, praid);
		// The assumption is that the surrounding objects (disk_segment & chunk) are also removed
		NFREE_PRAID(sdr43e9, praid);
		rv = 0;
	}
	NFOUT;
	return rv;
}

/* Should be matched with FREE_PRAID() */
#define NALLOCATE_PRAID(name)																\
({																							\
	struct nvmeibt_praid *__praid;															\
	__praid = NNVMEIBT_TOMA_CALLOC(name ## _calloc, 1, sizeof *__praid);					\
	XDLIST_INIT_LINK(&__praid->global_report_to_mgmt_praid_link, NULL);						\
	XDLIST_INIT_LINK(&__praid->praid_topo_recalc_link, NULL);								\
	XDLIST_HEAD_INIT(&__praid->praid_mgmt.all_segs_list);									\
	(__praid);																				\
})

static void build_praid_from_config(struct nvmeibt_praid_config *f, struct mm_praid_conf *conf, struct mm_vol_conf *vol)
{
	f->id = conf->uuid;
	f->was_ever_activated = conf->activated;
	if (vol->lockServer_type != OWNER_SCHEME_SL_START_DEC_C) {
		N_Ef(u83472a, "lock_scheme_type=@HEX_1B is not supported", vol->lockServer_type);
		nvmeibt_abort(ES_FATAL);
	}
	f->lock_scheme_type = vol->lockServer_type;
	f->redundancy = vol->lockServer_maxNOwners - 1;
	f->topo_config_idx_updated = conf->topo_config_idx_updated;	// Transfer TOPO_CONFIG version from persistence
}

enum nvmeibt_add_rv nvmeibt_praid_add(struct mm_praid_conf *conf,
									  struct nvmeibt_chunk *chunk,
									  struct mm_vol_conf *vol,
									  bool is_updating_leader,
									  struct nvmeibt_praid **praid_out,
									  int config_tag)
{
	enum nvmeibt_add_rv				rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_praid			*new_praid = NULL;	// Read into it, maybe use it.
	struct nvmeibt_praid			*praid;
	struct nvmeibt_disk_segment		*seg;
	int8_t							i;

	NFIN;

	praid = nvmeibt_praid_get_praid_by_id(&(conf->uuid));
	if (praid) {
		praid->trim_flags &= ~CONFIG_TRIM_MGMT;
		if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(praid)) { // the praid is reanimated, take the new config
			N_Tf(r9987b5, "praid=@UUID_LE praid->version=@X was outdated, recreating", &(vol->uuid), vol->version);
			praid->from_config.version = ILLEGAL_CONFIG_VER; // NNVMEIBT_HASH_ADD_OBJ will update it
			praid->config_tag = 0; // NNVMEIBT_HASH_ADD_OBJ will update it
		} else if (praid->from_config.version >= (int)vol->version) {
			N_Tf(cbsj7pm, "praid=@UUID_LE praid->version=@X received version=@X, skipping", &(conf->uuid), praid->from_config.version, vol->version);
			praid->config_tag = config_tag;
			rv = NVMEIBT_ADD_ALREADY_UP_TO_DATE;
			goto out;
		}
	}
	praid = NULL;

	new_praid = NALLOCATE_PRAID(o9i98n3);
	build_praid_from_config(&new_praid->from_config, conf, vol);
	new_praid->from_config.version = vol->version; //MGMT doesn't send us a vol version per praid

	rv = NNVMEIBT_HASH_ADD_OBJ_new(u87uy75,
							   nvmeibt_global_get_global()->praids_hash_by_uuid,
							   new_praid,
							   config_tag,
							   NVMEIBT_MAX_N_PRAIDS,
							   praid,
							   praid);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// In any case, update the following config-driven fields
	// None

	if (rv == NVMEIBT_ADD_NEW) {
		praid->praid_mgmt.urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&conf->uuid);
		switch(vol->raidType) {
		case 0: praid->praid_mgmt.type = NVMEIBT_PRAID_TYPE_JBOD;  break;
		case 1: praid->praid_mgmt.type = NVMEIBT_PRAID_TYPE_RAID1; break;
		case 6: praid->praid_mgmt.type = NVMEIBT_PRAID_TYPE_RAID6; break;
		}
		praid->praid_mgmt.stripe_idx = conf->stripeIndex;
		praid->praid_mgmt.chunk_id = chunk->from_config.id;
		praid->praid_mgmt.lockset_shift = vol->lockServer_locksetShift;
		praid->trim_flags = 0;
		praid->praid_mgmt.its_chunk = chunk;
		chunk->praids[praid->praid_mgmt.stripe_idx] = praid;
		praid_one_time_init(praid);
	}
	if (rv == NVMEIBT_ADD_NEW || rv == NVMEIBT_ADD_MODIFIED) {
		if (is_updating_leader)
			NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(at7776c, praid);
		for (i = 0; i < NVMEIBT_MAX_N_SEGMENTS_IN_PRAID; i++) {
			praid->praid_mgmt.topo_segs[i] = NULL;
			praid->praid_mgmt.replacement_topo_segs[i] = NULL;
		}
		praid->praid_mgmt.n_topo_segs = 0;
		XDLIST_FOREACH_SAFE(seg, &(praid->praid_mgmt.all_segs_list)) {
			// N_Tf(ttvJHGQ, "seg=@UUID_8 @PTR", nvmeibt_seg_UUID_8(seg), seg);
			XDLIST_DEL(&seg->praid_all_segs_link);
		}
	}
out:
	if (rv == NVMEIBT_ADD_NEW) {
		/* nothing */ ;
	} else {
		N_Tf(er45mc0, "Freeing unused praid");
		NFREE_PRAID(olp0fr9, new_praid);
	}

	*praid_out = praid;
	NFOUT;
	return rv;
}

void nvmeibt_praid_free_all_at_exit(void)
{
	struct nvmeibt_praid *praid;
	NVMEIB_HASH_FOREACH(praid, nvmeibt_global_get_global()->praids_hash_by_uuid) {
		nvmeibt_praid_remove(praid);
	}
}

static void praid_lot_forget_all_segs(struct nvmeibt_praid_lot *praid_lot)
{
	struct nvmeibt_seg_lot						*seg_lot;

	praid_lot->n_topo_seg_lots = 0;
	XDLIST_FOREACH_SAFE(seg_lot, &(praid_lot->all_seg_lot_list)) {
		XDLIST_DEL(&seg_lot->praid_all_seg_lots_link);
	}
}

void nvmeibt_praid_update_committed_lot_config(struct mm_praid_conf *conf, struct mm_vol_conf *vol, struct nvmeibt_praid **praid_out)
{
	struct nvmeibt_praid						*praid;
	struct nvmeibt_praid_lot					*praid_lot;
	int											i;

	praid = nvmeibt_praid_get_praid_by_id(&conf->uuid);
	if (!praid) {
		N_Wf(ry7764s, "Trying to commit not existing praid=@UUID_LE, probably in deletion", &conf->uuid);
		*praid_out = NULL;
		return;
	}

	praid_lot = &praid->praid_follower.committed_praid_lot;
	build_praid_from_config(&praid_lot->from_config, conf, vol);
	praid_lot->from_config.version = conf->version; //Leader send us a vol version per praid

	for (i = 0; i < NVMEIBT_MAX_N_SEGMENTS_IN_PRAID; i++) {
		praid_lot->topo_seg_lots[i] = NULL;
		praid_lot->replacement_topo_seg_lots[i] = NULL;
	}
	praid_lot_forget_all_segs(praid_lot);

//		XDLIST_HEAD_INIT(&praid_lot->all_seg_lot_list);

	N_Tf(gyy7712, "praid=@UUID_LE committed", nvmeibt_praid_UUID(praid));
	*praid_out = praid;
}

static void praid_forget_all_segs(struct nvmeibt_praid *praid)
{
	struct nvmeibt_disk_segment		*seg;

	XDLIST_FOREACH_SAFE(seg, &(praid->praid_mgmt.all_segs_list)) {
		if (seg->trim_flags == CONFIG_TRIM_ALL)
			NVMEIBT_OBJ_MARK_OUTDATED(vvsjewm, seg, seg);
		else
			N_Tf(9sm2lso, "don't delete seg=@UUID_8 llags=@X", nvmeibt_seg_UUID_8(seg), seg->trim_flags);
		XDLIST_DEL(&seg->praid_all_segs_link);
	}
	praid->praid_mgmt.n_topo_segs = 0;
	praid_lot_forget_all_segs(&praid->praid_leader.baseline_praid_lot);
	praid_lot_forget_all_segs(&praid->praid_leader.calculated_praid_lot);
	praid_lot_forget_all_segs(&praid->praid_leader.to_report_praid_lot);
	praid_lot_forget_all_segs(&praid->praid_follower.committed_praid_lot);
	praid_lot_forget_all_segs(&praid->praid_follower.applied_praid_lot);
}

void nvmeibt_praid_trim_specific_praid(struct nvmeibt_praid *praid, uint8_t trim_flag)
{
	if (is_trim_needed(&praid->trim_flags, trim_flag)) {
		// Ignoring unregister of clients. It is handled by the deleted segments
		NVMEIBT_OBJ_MARK_OUTDATED(dkitu83, praid, praid);
		praid_forget_all_segs(praid);
		// Let the standard conf & topo generations detect the OUTDATED and clean the serialized buffers
		nvmeibt_mm_jason_leader_topo_config_mm_praid_conf_and_mm_segs_conf_to_wire_buf(praid);
		praid_leader_serialize_topo(praid);
		//
		NVMEIBT_PRAID_CLEAR_TOPO_RECALC_REQUIRED(tye74ne, praid);
	}
	else {
		N_Tf(nssen92, "not yet, praid=@UUID_8 flags=@X", nvmeibt_praid_UUID_8(praid), praid->trim_flags);
	}
}

void nvmeibt_praid_trim_unused_entries(int config_tag, uint8_t trim_flag)
{
	struct nvmeibt_praid	*praid;

	NFIN;
	NVMEIB_HASH_FOREACH(praid, nvmeibt_global_get_global()->praids_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_OLDER(praid, config_tag)) {
			nvmeibt_praid_trim_specific_praid(praid, trim_flag);
		}
	}
	NFOUT;
}

void nvmeibt_praid_leader_we_have_a_new_baseline(struct nvmeibt_praid *praid, struct nvmeibt_praid_lot *src_praid_lot)
{
	struct nvmeibt_praid_leader				*praid_leader = &praid->praid_leader;
	bool									is_conf_changed;

	NFIN;
	is_conf_changed = (praid_leader->baseline_praid_lot.from_config.version != src_praid_lot->from_config.version);
	nvmeibt_praid_lot_duplicate_content(&praid_leader->baseline_praid_lot, src_praid_lot);

	//memset(&praid_leader->previous_topo_for_clients, 0, sizeof(praid_leader->previous_topo_for_clients));
	//nvmeibt_praid_lot_calc_topo_for_clients(&(praid_leader->baseline_praid_lot), &(praid_leader->previous_topo_for_clients), false);
	if (is_conf_changed) {
		leader_generate_topo_config_buf_of_praid_and_its_segs_mm_conf_from_baseline_praid_lot(praid);
		SET_RAFT_COMMIT_LIFECYCLE_VAL(sxro0n5, TOPO_CONFIG, leader_calculated, RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit) + 1);
	}
	praid_leader_serialize_topo(praid);
	getnstimeofday_boot(&(praid_leader->last_serialization_timestamp));
	NFOUT;
}

void nvmeibt_praid_mark_immediate_report_to_mgmt_required(struct nvmeibt_praid *praid)
{
	N_Tf(ju8y765, "praid=@UUID_LE", nvmeibt_praid_UUID(praid));
	if (XDLIST_NULL(&(praid->global_report_to_mgmt_praid_link))) {
		// Remove from periodic_list and add as last in immediate_list
		XDLIST_ADD_TAIL(&(nvmeibt_global_get_global()->immediate_report_to_mgmt_praid_list), praid);
	}
}

static void praid_just_did_report_to_mgmt(struct nvmeibt_praid *praid)
{
	N_Tf(bb74b5u, "praid=@UUID_LE", nvmeibt_praid_UUID(praid));
	if (!XDLIST_NULL(&(praid->global_report_to_mgmt_praid_link))) {
		// Remove from the current list (immediate/periodic) and add as last in periodic_list
		XDLIST_DEL(&(praid->global_report_to_mgmt_praid_link));
	}
}

const char *nvmeibt_mm_segment_persistent_status_to_str(enum SEGMENT_STATUS_FOR_MGMT n)
{
	// Note: the following strings are communicated with the management as-is. Do not change them!
	switch (n) {
	case SEGMENT_STATUS_FOR_MGMT_UNDER_RECOVERY:	return "under_recovery";
	case SEGMENT_STATUS_FOR_MGMT_NORMAL:			return "normal";
	case SEGMENT_STATUS_FOR_MGMT_DEPRECATED:		return "deprecated";
	case SEGMENT_STATUS_FOR_MGMT_DEAD:				return "dead";
	case SEGMENT_STATUS_FOR_MGMT_REPLACEMENT:		return "replacement";
	case SEGMENT_STATUS_FOR_MGMT_CONF_CORRUPTED:	return "conf_corrupted";
	case SEGMENT_STATUS_FOR_MGMT_BOOTING:			return "booting";
	case SEGMENT_STATUS_FOR_MGMT_ZEROING:			return "zeroing";
	case SEGMENT_STATUS_FOR_MGMT_INITIALIZING:		return "initializing";
	default:										return "unknown";
	}
}

const char *nvmeibt_mm_segment_vitality_to_str(enum SEGMENT_VITALITY_FOR_MGMT n)
{
	// Note: the following strings are communicated with the management as-is. Do not change them!
	switch (n) {
	case SEGMENT_VITALITY_DOWN: return         "down";
	case SEGMENT_VITALITY_UP: return "up";
	default: return "unknown";
	}
}

static enum SEGMENT_STATUS_FOR_MGMT calc_seg_lot_status_for_mgmt(struct nvmeibt_seg_lot *to_report_seg_lot, struct nvmeibt_disk_segment_topo_ctx *seg_topo_ctx,
																 bool is_praid_activated, bool is_praid_booting)
{
	if (nvmeibt_disk_segment_is_x_done(seg_topo_ctx))											return SEGMENT_STATUS_FOR_MGMT_DEPRECATED;
	if (nvmeibt_seg_lot_is_zeroing_explicitly_required_according_to_config(to_report_seg_lot))	return SEGMENT_STATUS_FOR_MGMT_ZEROING;
	if (!nvmeibt_seg_lot_is_config_OK(to_report_seg_lot))										return SEGMENT_STATUS_FOR_MGMT_CONF_CORRUPTED;
	if ((nvmeibt_disk_segment_is_calculated_dead(seg_topo_ctx) || !(is_praid_activated))) 		return SEGMENT_STATUS_FOR_MGMT_DEAD;
	if (nvmeibt_disk_segment_is_reported_as_under_recovery(seg_topo_ctx))   					return SEGMENT_STATUS_FOR_MGMT_UNDER_RECOVERY;
	if (nvmeibt_disk_segment_is_de_facto_owner(seg_topo_ctx)) {
		if (is_praid_booting)																	return SEGMENT_STATUS_FOR_MGMT_BOOTING;
		else																					return SEGMENT_STATUS_FOR_MGMT_NORMAL;
	}
	if (to_report_seg_lot->is_replacement)														return SEGMENT_STATUS_FOR_MGMT_REPLACEMENT;
	if (seg_topo_ctx->dirty_bits_state == NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN) {
		if (nvmeibt_seg_topo_is_newly_added(seg_topo_ctx)) {
			N_Tf(hu87ytf, "Just replaced seg");
		}
		else {
			N_Wf(h827ytf, "Suspicious unknown state");
		}
		return SEGMENT_STATUS_FOR_MGMT_INITIALIZING;
	}
	return SEGMENT_STATUS_FOR_MGMT_UNKNOWN;
}

static int8_t recalc_praid_report_to_mgmt_json(struct nvmeibt_praid *praid, struct nvmeibt_Str *json_payload)
{
	struct nvmeibt_praid_lot				*to_report_praid_lot;
	struct nvmeibt_seg_lot					*to_report_seg_lot;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_ctx;
	struct nvmeibt_disk_segment_topo_ctx	*seg_remote_topo_ctx;
	struct nvmeibt_praid_topo_ctx			*praid_topo_ctx;
	struct nvmeibt_disk						*disk;
	BOOL									is_praid_activated;
	BOOL									is_praid_booting, is_seg_booting;
	enum SEGMENT_STATUS_FOR_MGMT			new_reported_persistent_status;
	enum SEGMENT_VITALITY_FOR_MGMT			new_reported_vitality;

	TODO(Discuss with Tom and Yaniv, how we handle praid-down with no leader)
	//
	NFIN;

	to_report_praid_lot = &praid->praid_leader.to_report_praid_lot;
	praid_topo_ctx = &to_report_praid_lot->topo_ctx;
	is_praid_booting = nvmeibt_praid_topo_is_client_sync_cmd_cold_recovery(praid_topo_ctx->registrants_sync_cmd);
	is_praid_activated = (praid_topo_ctx && praid_topo_ctx->is_activated);
	nvmeibt_Str_sprintf(json_payload,
						"{\"uuid\": \"%s\", "
						"\"raftTerm\": %d, "
						"\"pRaidMinorVersion\": %d, "
						"\"pRaidMajorVersion\": %d, "
						"\"isRaftLeader\": %d, "
						"\"segments\": [",
						nvmeibt_praid_id_str(to_report_praid_lot->my_praid),
						nvmeibt_raft_get_current_term(),
						praid_topo_ctx->praid_version_minor,
						praid_topo_ctx->praid_version_major,
						nvmeibt_raft_is_leader());
	XDLIST_FOREACH_SAFE(to_report_seg_lot, &(to_report_praid_lot->all_seg_lot_list)) {
		disk = nvmeibt_seg_lot_get_disk(to_report_seg_lot);
		seg_topo_ctx = &to_report_seg_lot->seg_topo;
		seg_remote_topo_ctx = &(to_report_seg_lot->my_seg->seg_leader.remote_seg_topo);

		is_seg_booting = is_praid_booting |
						 (nvmeibt_disk_segment_leader_is_remote_active_mem_tbl_init_command(seg_topo_ctx) &&
						  nvmeibt_disk_segment_is_de_facto_owner(seg_topo_ctx));
		new_reported_persistent_status = calc_seg_lot_status_for_mgmt(to_report_seg_lot, seg_topo_ctx, is_praid_activated, is_seg_booting);
		new_reported_vitality = (nvmeibt_raft_is_shutdown_triggered() ||
								 nvmeibt_disk_segment_is_dirty_bits_state_down(seg_remote_topo_ctx->dirty_bits_state) ||
								 !disk ||
								 !disk->leader_its_raft_member) ?
			 SEGMENT_VITALITY_DOWN : SEGMENT_VITALITY_UP;
		nvmeibt_Str_sprintf(json_payload,
							"{\"segmentID\":\"%s\","
							"\"status\":\"%s\","
							"\"vitality\":\"%s\"},",
							nvmeibt_seg_lot_id_str(to_report_seg_lot),
							nvmeibt_mm_segment_persistent_status_to_str(new_reported_persistent_status),
							nvmeibt_mm_segment_vitality_to_str(new_reported_vitality));
	}
	if (XDLIST_N_ELEMNTS(&(to_report_praid_lot->all_seg_lot_list))) {
		nvmeibt_Str_chop_last_char(json_payload); // Remove the last "," after the last seg report
	}
	nvmeibt_Str_sprintf(json_payload, "]},");

	NFOUT;
	return (int8_t)XDLIST_N_ELEMNTS(&(to_report_praid_lot->all_seg_lot_list));
}

int8_t nvmeibt_praid_append_to_report_to_mgmt(struct nvmeibt_praid *praid, struct nvmeibt_Str *json_payload) {
	const int8_t n_segs_in_report = recalc_praid_report_to_mgmt_json(praid, json_payload);
	const int pre_len = nvmeibt_Str_strlen(json_payload);
	NVMEIBT_IMPORTANT_LOGS_DUMP_LEADER_REPORT_LINE_FOR_VOLUME(5vhjsi8, praid->praid_mgmt.its_chunk->its_block_device->from_config.client_blkdev_name,
															  nvmeibt_Str_str(json_payload) + pre_len);
	TODO(With KAFKA we would like to give each praid report a praid_id key, so that KAFKA can delete old praid reports. Need to separate them for this);
	praid_just_did_report_to_mgmt(praid);
	return n_segs_in_report;
}

static void dump_praid_seg_to_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_disk_segment *seg,
									 struct nvmeibt_disk_segment_topo_ctx *G_ctx, struct nvmeibt_disk_segment_topo_ctx *R_ctx)
{
	if (seg) {
		(*printf_fn)(printf_ctx, "\t\t\t\t- seg=%.8s node=%s state(%s is_synchronizer=%d dep=%c%s) Remote[praid_ver=%x.%x are_reg_sync=%d dirty_init_mode=%s]\n",
				nvmeibt_disk_segment_id_str(seg),
				nvmeibt_disk_get_applied_node_name(seg->seg_mgmt.its_disk),
				dirty_bits_state_str(G_ctx->dirty_bits_state),
				G_ctx->is_registrants_synchronizer,
				seg->from_config.deprecation_flag,
				(nvmeibt_disk_segment_is_config_OK(seg) ? "" : " !is_config_OK"),
				(R_ctx ? R_ctx->seg_praid_version_major : -1), (R_ctx ? R_ctx->seg_praid_version_minor : -1),
				(R_ctx ? R_ctx->active_seg_flags.are_praid_registrants_aligned_with_sync_cmd : -1),
				mem_tbl_init_mode_str(R_ctx ? R_ctx->dirty_bits_init_mode : NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN));
	}
	else {
		(*printf_fn)(printf_ctx, "\t\t\t\t- seg=NULL\n");
	}
}

int nvmeibt_praid_dump_praid_status_line(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid *praid, BOOL is_leader, bool is_full_info_needed)
{
	struct nvmeibt_praid_topo_ctx	*praid_ctx;

	if (!praid) {
		(*printf_fn)(printf_ctx, "\t\t\t- OOPS praid=NULL\n");
		goto out;
	}
	praid_ctx = (is_leader ? &(praid->praid_leader.baseline_praid_lot.topo_ctx) : &(praid->praid_follower.applied_praid_lot.topo_ctx));
	if (is_full_info_needed) {
		(*printf_fn)(printf_ctx, "\t\t\t- praid=%s Type=%s ", nvmeibt_praid_id_str(praid), nvmeibt_praid_type_str(praid->praid_mgmt.type));
	} else {
		(*printf_fn)(printf_ctx, "\t\t\t- ");
	}
	(*printf_fn)(printf_ctx, "praid_ver=%x.%x sync_cmd=%s are_reg_sync=%d is_activated=%d topo_idx_updated=%"PRIx64"%s%s\n",
			praid_ctx->praid_version_major, praid_ctx->praid_version_minor,
			praid_registrants_sync_cmd_str(praid_ctx->registrants_sync_cmd),
			praid_ctx->leader_did_all_segs_sync_registrants,
			praid_ctx->is_activated,
			praid_ctx->topo_idx_updated,
			(nvmeibt_praid_is_being_deleted(praid) ? " being_deleted" : ""),
			(nvmeibt_praid_is_conf_corrupted(praid) ? " conf_corrupted" : ""));
out:
	return 0;
}

int nvmeibt_praid_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid *praid)
{
	struct nvmeibt_disk_segment			*seg;
	struct nvmeibt_seg_lot				*seg_lot;
	struct nvmeibt_praid_lot			*praid_lot;
	bool								is_leader = nvmeibt_raft_is_leader();

	NFIN;
	if (!praid) {
		(*printf_fn)(printf_ctx, "\t\t\t- OOPS praid=NULL\n");
		goto out;
	}
	nvmeibt_praid_dump_praid_status_line(printf_fn, printf_ctx, praid, is_leader, 1);
	praid_lot = is_leader ? &praid->praid_leader.baseline_praid_lot : &praid->praid_follower.applied_praid_lot;

	XDLIST_FOREACH_SAFE(seg_lot, &(praid_lot->all_seg_lot_list)) {
		seg = seg_lot->my_seg;
		dump_praid_seg_to_status(printf_fn, printf_ctx, seg, &seg_lot->seg_topo, is_leader ? &seg->seg_leader.remote_seg_topo : NULL);
	}

out:
	NFOUT;
	return 0;
}

void nvmeibt_praid_print_leader_wire_topo_with_segs(struct nvmeibt_praid_leader *praid_leader)
{
	struct nvmeibt_serialized_seg_leader_topo		*seg_wire_topo_ptr;
	struct nvmeibt_Str 								*print_s;
	char											i;

	print_s = NNVMEIBT_STR_ALLOC(qmri104);
	NNVMEIBT_STR_RESIZE_BUF(fhu2i8q, print_s, 2048);

	nvmeibt_praid_print_leader_wire_topo((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, &(praid_leader->praid_wire_topo));

	seg_wire_topo_ptr = (struct nvmeibt_serialized_seg_leader_topo *)praid_leader->segs_wire_topo_buf.data_buf;
	for (i = 0; i < LE_SWAP8(praid_leader->praid_wire_topo.segs_num); i++)
	{
		nvmeibt_Str_sprintf(print_s,"   ");
		nvmeibt_disk_segment_print_leader_wire_topo((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, seg_wire_topo_ptr);
		seg_wire_topo_ptr++;
	}

	N_Tf(ten12jo, "\nSERIALIZED PRAID TOPOLOGY:\n@STR", nvmeibt_Str_str(print_s));
	NNVMEIBT_STR_FREE(eu19zmg, print_s);
}

/*------------------lot functions------------------*/

static struct nvmeibt_seg_lot *praid_lot_get_my_seg_lot_by_seg(struct nvmeibt_disk_segment *seg, struct nvmeibt_praid_lot *praid_lot)
{
	return (struct nvmeibt_seg_lot *)((unsigned long long)seg + praid_lot->my_seg_lot_offset);
}

static inline char get_praid_topo_char(struct nvmeibt_praid_lot *praid_lot)
{
	struct nvmeibt_praid			*praid = praid_lot->my_praid;

    return (&praid->praid_leader.calculated_praid_lot == praid_lot ?	'L' :
			&praid->praid_leader.baseline_praid_lot == praid_lot ?		'B' :
			&praid->praid_leader.to_report_praid_lot == praid_lot ?		'M' :
			&praid->praid_follower.committed_praid_lot == praid_lot ?	'C' :
			&praid->praid_follower.applied_praid_lot == praid_lot ?		'P' :
			'U');
}


void nvmeibt_praid_lot_duplicate_content(struct nvmeibt_praid_lot *praid_lot_dst, struct nvmeibt_praid_lot *praid_lot_src)
{
	struct nvmeibt_seg_lot					*seg_lot_dst;
	struct nvmeibt_seg_lot					*seg_lot_src;
	struct nvmeibt_seg_lot					*owner_seg_lot;
	int8_t									idx_in_topo_segs;
	struct nvmeibt_disk_segment_topo_ctx	*seg_topo_dst;
	//int										i, n_seg_lots;
	bool									is_x_done_restore_needed;
	bool									is_dst_praid_lot_leader_calculated_lot;

	NFIN;
	N_Tf(7bsc2of, "praid=@UUID_LE config_version=@X (@CHAR->@CHAR)",
		 nvmeibt_praid_lot_UUID(praid_lot_src), praid_lot_src->from_config.version,
		 get_praid_topo_char(praid_lot_src), get_praid_topo_char(praid_lot_dst));
	// Decide what we want to keep from the old config, and free the rest
	if (praid_lot_dst->from_config.version != praid_lot_src->from_config.version) {
		// Different configuration
		XDLIST_FOREACH_SAFE(seg_lot_dst, &(praid_lot_dst->all_seg_lot_list)) {
			XDLIST_DEL(&seg_lot_dst->praid_all_seg_lots_link);
		}
		praid_lot_dst->from_config = praid_lot_src->from_config;
		praid_lot_dst->n_topo_seg_lots = praid_lot_src->n_topo_seg_lots;

		// Generate all the dst seg_lot objects and embed them in the praid_lot_dst
		XDLIST_FOREACH_SAFE(seg_lot_src, &(praid_lot_src->all_seg_lot_list)) {
			idx_in_topo_segs = seg_lot_src->from_config.idx_in_praid;
			seg_lot_dst = praid_lot_get_my_seg_lot_by_seg(seg_lot_src->my_seg, praid_lot_dst);
			XDLIST_ADD_TAIL(&(praid_lot_dst->all_seg_lot_list), seg_lot_dst);
			seg_lot_dst->from_config = seg_lot_src->from_config;
			seg_lot_dst->my_seg = seg_lot_src->my_seg;
			if (seg_lot_src->is_replacement) {
				N_Tf(asiw3j5, "replacement_seg_lot[@INT]=@UUID_8", idx_in_topo_segs, nvmeibt_seg_lot_UUID_8(seg_lot_dst));
				praid_lot_dst->replacement_topo_seg_lots[idx_in_topo_segs] = seg_lot_dst;
			} else {
				N_Tf(asiw3j7, "seg_lot[@INT]=@UUID_8", idx_in_topo_segs, nvmeibt_seg_lot_UUID_8(seg_lot_dst));
				praid_lot_dst->topo_seg_lots[idx_in_topo_segs] = seg_lot_dst;
			}
		}
	}
	else {
		N_Tf(ryyyu12,"same config_version");
	}

	// praid_lot_dst->praid_lot_src = praid_lot_src;
	N_Tf(uuu8xd3, "src=(@PRAID_VERSION,@PRAID_VERSION) dst=(@PRAID_VERSION,@PRAID_VERSION)",
		 praid_lot_src->topo_ctx.praid_version_major,
		 praid_lot_src->topo_ctx.praid_version_minor,
		 praid_lot_dst->topo_ctx.praid_version_major,
		 praid_lot_dst->topo_ctx.praid_version_minor);
	N_Tf(uuuax01, "src topo_idx_updated=@INT64_TX dst topo_idx_updated=@INT64_TX",
		praid_lot_src->topo_ctx.topo_idx_updated,
		praid_lot_dst->topo_ctx.topo_idx_updated);
	praid_lot_dst->topo_ctx = praid_lot_src->topo_ctx;
	is_dst_praid_lot_leader_calculated_lot = (&(praid_lot_dst->my_praid->praid_leader.calculated_praid_lot) == praid_lot_dst);
	XDLIST_FOREACH_SAFE(seg_lot_dst, &(praid_lot_dst->all_seg_lot_list)) {
		seg_lot_src = praid_lot_get_my_seg_lot_by_seg(seg_lot_dst->my_seg, praid_lot_src);
		seg_lot_dst->is_replacement = seg_lot_src->is_replacement;

		// Don't lose X_DONE that is already in life cycle
		is_x_done_restore_needed = is_dst_praid_lot_leader_calculated_lot ? 0 : nvmeibt_disk_segment_is_x_done(&(seg_lot_dst->seg_topo));

		N_Tf(ryyy711, "seg=@UUID_8 src=(@PRAID_VERSION,@PRAID_VERSION) dst=(@PRAID_VERSION,@PRAID_VERSION)",
			 nvmeibt_seg_lot_UUID_8(seg_lot_dst),
			 seg_lot_src->seg_topo.seg_praid_version_major,
			 seg_lot_src->seg_topo.seg_praid_version_minor,
			 seg_lot_dst->seg_topo.seg_praid_version_major,
			 seg_lot_dst->seg_topo.seg_praid_version_minor);
		seg_lot_dst->seg_topo = seg_lot_src->seg_topo;
		if (is_x_done_restore_needed) {
			seg_lot_dst->seg_topo.dirty_bits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;
		}
		// seg_lot_dst->is_FIRST_USE_EVER. Interesting when copying baseline-->calculated
		if (nvmeibt_disk_segment_is_mem_tbl_init_beyond_FIRST_USE_EVER(&(seg_lot_dst->seg_topo))) {
			seg_lot_dst->is_FIRST_USE_EVER = 0;
		} else if (nvmeibt_disk_segment_is_mem_tbl_init_FIRST_USE_EVER(&(seg_lot_dst->seg_topo))) {
			seg_lot_dst->is_FIRST_USE_EVER = 1;
		} else {
			// Leave as is, src (baseline or committed) did not hold the entire picture (probably some kind of UNKNOWN, possibly a theoretic case)
		}
		// Update links to owners
		seg_topo_dst = &seg_lot_dst->seg_topo;
		owner_seg_lot = seg_topo_dst->owner_seg_lot;
		if (owner_seg_lot) {
			owner_seg_lot = praid_lot_get_my_seg_lot_by_seg(owner_seg_lot->my_seg, praid_lot_dst);
			seg_topo_dst->owner_seg_lot = owner_seg_lot;
		} else {
			seg_topo_dst->owner_seg_lot = NULL;
		}
		owner_seg_lot = seg_topo_dst->secondary_owner_seg_lot;
		if (owner_seg_lot) {
			owner_seg_lot = praid_lot_get_my_seg_lot_by_seg(owner_seg_lot->my_seg, praid_lot_dst);
			seg_topo_dst->secondary_owner_seg_lot = owner_seg_lot;
		} else {
			seg_topo_dst->secondary_owner_seg_lot = NULL;
		}
	}

	NFOUT;
}

bool nvmeibt_praid_upd_calculated_lot_from_praid_mgmt(struct nvmeibt_praid *praid)
{
	struct nvmeibt_praid_mgmt			*praid_mgmt = &praid->praid_mgmt;
	struct nvmeibt_praid_leader			*praid_leader = &praid->praid_leader;
	struct nvmeibt_praid_lot			*calculated_praid_lot = &praid_leader->calculated_praid_lot;
	struct nvmeibt_disk_segment			*seg;
	struct nvmeibt_seg_lot				*calculated_seg_lot;
	struct nvmeibt_seg_lot				*baseline_seg_lot;
	int8_t								idx_in_topo_segs;
	bool								is_conf_change_requires_new_topo = 0;

	NFIN;
	N_Tf(6aoebof, "praid=@UUID_LE config_version=@X (@CHAR)", nvmeibt_praid_UUID(praid), praid->from_config.version, get_praid_topo_char(calculated_praid_lot));
	if (calculated_praid_lot->from_config.version == praid->from_config.version) {
		N_Tf(viekso3,"same config_version");
		goto out;
	}

	if (nvmeibt_praid_is_conf_corrupted(praid)) {
		N_Tf(i989ie4, "Conf corrupted, don't update ! conf=@INT", calculated_praid_lot->from_config.version);
		goto out;
	}

	// This praid's config is changing - it will be part of the next TOPO_CONFIG commit
	praid->from_config.topo_config_idx_updated = RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit) + 1;

	calculated_praid_lot->n_topo_seg_lots = praid_mgmt->n_topo_segs;
	calculated_praid_lot->from_config = praid->from_config;
	XDLIST_FOREACH_SAFE(calculated_seg_lot, &(calculated_praid_lot->all_seg_lot_list)) {
		XDLIST_DEL(&calculated_seg_lot->praid_all_seg_lots_link);
	}

	// Go over the mgmt segs and update
	XDLIST_FOREACH_SAFE(seg, &(praid_mgmt->all_segs_list)) {
		calculated_seg_lot = praid_lot_get_my_seg_lot_by_seg(seg, calculated_praid_lot);
		XDLIST_ADD_TAIL(&(calculated_praid_lot->all_seg_lot_list), calculated_seg_lot);
		idx_in_topo_segs = seg->from_config.idx_in_praid;

		baseline_seg_lot = praid_lot_get_my_seg_lot_by_seg(seg, &praid_leader->baseline_praid_lot);
		if (baseline_seg_lot->from_config.version == ILLEGAL_CONFIG_VER) {
			// A new seg_lot
			calculated_seg_lot->is_FIRST_USE_EVER = 1;
			is_conf_change_requires_new_topo = 1;
			calculated_seg_lot->is_replacement = seg->seg_mgmt.is_replacement;
			if (calculated_seg_lot->is_replacement) {
				N_Tf(jur8uw3, "replacement_seg_lot[@INT]=@UUID_8", idx_in_topo_segs, nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
				calculated_praid_lot->replacement_topo_seg_lots[idx_in_topo_segs] = calculated_seg_lot;
				//seg_lot->seg_topo.is_newly_added_seg = 1;
			}
			else {
				N_Tf(fhu876s, "seg_lot[@INT]=@UUID_8", idx_in_topo_segs, nvmeibt_seg_lot_UUID_8(calculated_seg_lot));
				calculated_praid_lot->topo_seg_lots[idx_in_topo_segs] = calculated_seg_lot;
			}
		}
		else {
			// An existing seg_lot. Remove replacement seg_lot if replacement occured
			if (calculated_seg_lot->is_replacement && !(seg->seg_mgmt.is_replacement)) {
				// The old 'R' seg was removed from config, we are no more replacement
				is_conf_change_requires_new_topo = 1;
				leader_switch_to_replacement_seg(calculated_praid_lot, idx_in_topo_segs);
			}
		}
		calculated_seg_lot->from_config = seg->from_config;
	}

out:
	NFOUT;
	return is_conf_change_requires_new_topo;
}

union io_perms_bitfield nvmeibt_praid_calc_io_perms(struct nvmeibt_praid *praid, bool is_during_cold_recovery)
{
	union io_perms_bitfield				io_perms;

	io_perms.all = 0;
	if (nvmeibt_praid_is_jbod(praid)){
		io_perms.bits.is_io_R = 1;
		io_perms.bits.is_io_W = 1;
	} else {
		io_perms.bits.is_cold_recovery = is_during_cold_recovery;
		io_perms.bits.is_io_R = !(io_perms.bits.is_cold_recovery);
		io_perms.bits.is_io_W = io_perms.bits.is_io_R;
		io_perms.bits.is_hot_recovery = 1;
		io_perms.bits.is_jgc_recovery = !(io_perms.bits.is_cold_recovery);
	}
	N_Tf(hruiswa, "praid=@UUID_8 io_perms=@X", nvmeibt_praid_UUID_8(praid), io_perms.all);
	return io_perms;
}

static char seg_deprecation_flag(struct nvmeibt_disk_segment *seg)
{
	return (seg ? seg->from_config.deprecation_flag : '?');
}
static int seg_is_replacement(struct nvmeibt_disk_segment *seg)
{
	return (seg ? seg->seg_mgmt.is_replacement : 0);
}

int nvmeibt_praid_validate_replacement_segs(struct nvmeibt_praid *praid)
{
	struct nvmeibt_praid_mgmt		*praid_mgmt;
	struct nvmeibt_disk_segment		*seg, *rep_seg;
	int								i;
	int								n_rep_seg = 0;

	praid_mgmt = &praid->praid_mgmt;
	for (i = 0; i < praid_mgmt->n_topo_segs; i++) {
		seg = praid_mgmt->topo_segs[i];
		rep_seg = praid_mgmt->replacement_topo_segs[i];
		if (rep_seg) {
			if (seg) {
				n_rep_seg++;
			} else {
				N_Tf(mssiii2, "MGMT switch to new seg=@UUID_8 idx=@INT", nvmeibt_seg_UUID_8(rep_seg), i);
				seg = rep_seg;
				rep_seg = NULL;
				praid_mgmt->topo_segs[i] = seg;
				praid_mgmt->replacement_topo_segs[i] = NULL;
				seg->from_config.deprecation_flag = 'N';
				seg->seg_mgmt.is_replacement = 0;
			}
		}

		if (seg && (seg_deprecation_flag(seg) != 'X')) {
			if ((!rep_seg && (seg_deprecation_flag(seg) != 'N')) ||
				(rep_seg && ((seg_deprecation_flag(seg) != 'R') || (seg_deprecation_flag(rep_seg) != 'S')))) {
				nvmeibt_praid_mark_conf_corrupted(praid);
				N_Ef(yabeko3, "seg=@UUID_8(deprecation_flag=@CHAR is_replacement=@BOOL) rep_seg=@UUID_8(deprecation_flag=@CHAR is_replacement=@BOOL)",
					 nvmeibt_seg_UUID_8(seg), seg_deprecation_flag(seg), seg_is_replacement(seg),
					 nvmeibt_seg_UUID_8(rep_seg), seg_deprecation_flag(rep_seg), seg_is_replacement(rep_seg));
				break;
			}
		}
	}
	return n_rep_seg;
}
