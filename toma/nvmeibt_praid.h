/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_PRAID
#define NVMEIBT_PRAID

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"

#include "clnt/nvmeibt_client_protocol.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_disk_segment.h"
#include "vol/nvmeibt_chunk.h"

/*
 * nvmeibt_praid Information Flow:
 *
 * nvmeibt_praid represents praid state in TOMA (stored in global praids_hash).
 * It contains configuration and topology for segments.
 * Data flows differently between leader and follower nodes.
 *
 * THREE BUFFERS per praid:
 *   1. KAFKA_MGMT_CONFIG: praid config (from vol config, bdev->kafka_mgmt_config_...wire_conf_buf)
 *   2. TOPO_CONFIG:       praid config + seg configs (praid_leader.topo_config_...wire_conf_buf)
 *   3. TOPO:              praid topo + seg topos (praid_leader.praid_wire_topo + segs_wire_topo_buf)
 *
 * DATA FLOW SUMMARY:
 *   Leader:   from_config + praid_mgmt -> calculated -> baseline -> [Raft wire bufs]
 *   Follower: [Raft wire bufs] -> committed -> applied -> active_seg_topo
 *
 * ========================= LEADER FLOW =========================
 * Mgmt sends config via Kafka
 *   |
 *   └─> nvmeibt_read_config_apply_vol_mgmt_conf()
 *       Populate from_config (praid config from vol config)
 *       Populate praid_mgmt (segments array, topo_segs[])
 *       Populate bdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf
 *   |
 *   └─> nvmeibt_topology_calc_topology() triggered
 *       |
 *       ├─> nvmeibt_praid_upd_calculated_lot_from_praid_mgmt()
 *       |   baseline_praid_lot + from_config + praid_mgmt.topo_segs[] -> calculated_praid_lot
 *       |
 *       ├─> nvmeibt_praid_leader_calc_topo_main()
 *       |   Calculate seg states, owners, init_modes, praid_version
 *       |   Updates calculated_praid_lot (topo_ctx, seg_lot.seg_topo)
 *       |
 *       └─> nvmeibt_praid_leader_we_have_a_new_baseline()
 *           calculated_praid_lot -> baseline_praid_lot
 *           |
 *           ├─> praid_leader_serialize_topo()
 *           |   baseline_praid_lot -> praid_wire_topo + segs_wire_topo_buf
 *           |   (Contains: registrants_sync_cmd, dirty_bits_state, init_modes, owners)
 *           |
 *           └─> leader_generate_topo_config_buf_of_praid_and_its_segs_mm_conf_from_baseline_praid_lot()
 *               baseline_praid_lot -> topo_config_praid_and_segs_wire_conf_buf
 *               (Contains: praid version, seg configs with deprecation_flag)
 *   |
 *   └─> Assemble into persist_and_wire_buf:
 *       TLV[TOPO]:        from praid_wire_topo + segs_wire_topo_buf
 *       TLV[TOPO_CONFIG]: from topo_config_praid_and_segs_wire_conf_buf
 *       TLV[MGMT_CONFIG]: from kafka_mgmt_config_...wire_conf_buf
 *   |
 *   └─> Send APPEND_ENTRIES to followers (one message with all three TLVs)
 *   |
 *   └─> Upon majority commit: baseline_praid_lot -> to_report_praid_lot
 *       Used by nvmeibt_global_issue_leader_report_praids_status_to_mgmt()
 *       to send "updatePRaidReport" to Kafka for mgmt monitoring
 *
 * ======================== FOLLOWER FLOW ========================
 * Receive APPEND_ENTRIES from leader
 *   |
 *   └─> Persist follower_to_commit_persist_and_wire_buf_full to disk
 *   |
 *   └─> Parse THREE buffers into committed structures:
 *       [KAFKA_MGMT_CONFIG] -> from_config + praid_mgmt (nvmeibt_read_config_apply_vol_mgmt_conf)
 *       [TOPO_CONFIG]       -> committed_praid_lot.from_config (nvmeibt_read_config_apply_vol_committed_topo_conf)
 *       [TOPO]              -> committed_praid_lot.topo_ctx + seg_topo (nvmeibt_praid_upd_committed_topo)
 *   |
 *   └─> Apply to active (update_applied_topology):
 *       committed_praid_lot -> applied_praid_lot
 *   |
 *   └─> Update local seg_active (nvmeibt_seg_active_upd_active_topo_from_applied_topo):
 *       applied_praid_lot -> seg_active->active_seg_topo
 *       (Used for client registration decisions and local operations)
 */

struct nvmeibt_praid_mgmt {
	XDLIST_DECLARE(, struct nvmeibt_disk_segment, praid_all_segs_link) all_segs_list;
	struct nvmeibt_urn_uuid						urn_uuid;
	union nvmeib_uuid							chunk_id;
	struct nvmeibt_disk_segment					*topo_segs[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
	struct nvmeibt_disk_segment					*replacement_topo_segs[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
	struct nvmeibt_chunk						*its_chunk;
	int 										lockset_shift;	// Usually called "stride"
	int											stripe_idx;
	int8_t										n_topo_segs;
	enum NVMEIBT_PRAID_TYPE						type;
	BOOL										is_conf_corrupted;
};

struct nvmeibt_praid_lot {
	XDLIST_DECLARE(, struct nvmeibt_seg_lot, praid_all_seg_lots_link) all_seg_lot_list;
	struct nvmeibt_praid_config					from_config;
	struct nvmeibt_seg_lot						*topo_seg_lots[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
	struct nvmeibt_seg_lot						*replacement_topo_seg_lots[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID];
	struct nvmeibt_praid						*my_praid;
	struct nvmeibt_praid_topo_ctx				topo_ctx;
	int8_t										n_topo_seg_lots;
	int											my_seg_lot_offset;
};

struct nvmeibt_praid_leader {
	struct nvmeibt_praid_lot					baseline_praid_lot;
	struct nvmeibt_praid_lot					calculated_praid_lot;
	struct nvmeibt_praid_lot					to_report_praid_lot;

	struct tTopoOfPraid							previous_topo_for_clients;
	struct timespec								first_activation_attempt_timespec;

	struct timespec								last_serialization_timestamp;
	struct mm_praid_conf						serialized_topo_config_praid_and_its_segs_arr_conf;
	struct nvmeibt_Buf							topo_config_array_of_its_serialized_segs_conf;
	struct nvmeibt_praid_serialized_topo		praid_wire_topo;
	struct nvmeibt_Buf							segs_wire_topo_buf;
	struct nvmeibt_Buf							topo_config_praid_and_segs_wire_conf_buf;
	int											serialized_version_major;
	int											serialized_version_minor;

	BOOL										is_waiting_for_timeout_since_activation_attempt;
	BOOL										is_waiting_for_any_remote_seg_to_apply_topo;
    BOOL										did_any_client_report_about_problems;
};

struct nvmeibt_praid_follower {
	struct nvmeibt_praid_lot					committed_praid_lot;
	struct nvmeibt_praid_lot					applied_praid_lot;
	union io_perms_bitfield						applied_io_perms;
	BOOL										is_serialized_in_incremental_merge;	// Temporary flag for incremental merge (both TOPO and TOPO_CONFIG)
};

struct nvmeibt_praid {
	struct nvmeibt_praid_config					from_config;
	struct nvmeibt_praid_mgmt					praid_mgmt;
	struct nvmeibt_praid_leader					praid_leader;
	struct nvmeibt_praid_follower				praid_follower;
	struct xdlist								global_report_to_mgmt_praid_link;
	struct xdlist								praid_topo_recalc_link;
	char										vol_name_for_recovery[32];
	union nvmeib_uuid							vol_uuid_for_recovery;
	int											config_tag;
	int											n_recoveries_needing_hidden_attach;
	BOOL										was_praid_ever_activated;	// True only after there are traces in persistency (global_topo)
	uint8_t										trim_flags;
};

const char *nvmeibt_praid_get_type_str(const struct nvmeibt_praid *praid);

#define ILLEGAL_SEGS_NUM	((int8_t)-1)
#define ILLEGAL_STRIPE_INDEX	255

void nvmeibt_praid_inc_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid);
void nvmeibt_praid_dec_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid);
int nvmeibt_praid_get_n_recoveries_needing_hidden_attach(struct nvmeibt_praid *praid);

static inline struct nvmeibt_praid_topo_ctx *nvmeibt_praid_get_applied_topo(struct nvmeibt_praid *praid)
{
	return (praid ? &(praid->praid_follower.applied_praid_lot.topo_ctx) : NULL);
}

static inline struct nvmeibt_praid_lot *nvmeibt_praid_get_applied_praid_lot(struct nvmeibt_praid *praid)
{
	return (praid ? &praid->praid_follower.applied_praid_lot : NULL);
}

static inline BOOL nvmeibt_praid_is_jbod(struct nvmeibt_praid *praid)
{
	return (praid && (praid->praid_mgmt.type == NVMEIBT_PRAID_TYPE_JBOD));
}

static inline BOOL nvmeibt_praid_is_serialized(struct nvmeibt_praid *praid)
{
	return (praid && (praid->praid_leader.praid_wire_topo.segs_num != LE_SWAP8(ILLEGAL_SEGS_NUM)));
}

static inline BOOL nvmeibt_praid_is_conf_corrupted(const struct nvmeibt_praid *praid)
{
	return (praid && praid->praid_mgmt.is_conf_corrupted);
}

static inline BOOL nvmeibt_praid_is_segments_dirty_bit_relevant(struct nvmeibt_praid *praid)
{
	return (praid && !nvmeibt_praid_is_jbod(praid) && !nvmeibt_praid_is_conf_corrupted(praid));
}

static inline bool nvmeibt_praid_is_type_RAID1(const struct nvmeibt_praid *praid)
{
	return (praid && (praid->praid_mgmt.type == NVMEIBT_PRAID_TYPE_RAID1));
}

static inline bool nvmeibt_praid_is_type_EC(const struct nvmeibt_praid *praid)
{
	return (praid && nvmeibt_protocol_is_praid_type_journalled(praid->praid_mgmt.type));
}

static inline const union nvmeib_uuid *nvmeibt_praid_UUID(const struct nvmeibt_praid *praid)
{
	return (praid ? &praid->from_config.id : &nvmeib_uuid_null_val);
}

static inline unsigned int nvmeibt_praid_UUID_8(struct nvmeibt_praid *praid)
{
	return nvmeib_uuid_first_4_bytes(nvmeibt_praid_UUID(praid));
}

static inline const union nvmeib_uuid *nvmeibt_praid_lot_UUID(struct nvmeibt_praid_lot *praid_lot)
{
	return nvmeibt_praid_UUID(praid_lot->my_praid);
}

static inline bool nvmeibt_disk_segment_is_carrying_advanced_praid_version(struct nvmeibt_praid_topo_ctx *p, struct nvmeibt_disk_segment_topo_ctx *s)
{
	return ((p->praid_version_major < s->seg_praid_version_major) ||
			((p->praid_version_major == s->seg_praid_version_major) && (p->praid_version_minor < s->seg_praid_version_minor)));
}

static inline char *nvmeibt_praid_id_str(struct nvmeibt_praid *praid)
{
	return (praid ? praid->praid_mgmt.urn_uuid.str : "");
}

void nvmeibt_topology_leader_mark_recalc_required(void);
#define NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(name, _praid_) do {								\
	struct nvmeibt_praid	*_praid = (_praid_);												\
	if (_praid && XDLIST_NULL(&(_praid->praid_topo_recalc_link)) &&								\
		!NVMEIBT_OBJ_IS_MARKED_OUTDATED(_praid)) {											\
		N_Tf(name, "praid=@UUID_LE MARK_TOPO_RECALC_REQUIRED", nvmeibt_praid_UUID(_praid));		\
		XDLIST_ADD_TAIL(&(nvmeibt_global_get_global()->praid_topo_recalc_list), _praid);		\
		nvmeibt_topology_leader_mark_recalc_required();											\
	}																							\
} while (0)

#define NVMEIBT_PRAID_CLEAR_TOPO_RECALC_REQUIRED(name, _praid_) do {							\
	struct nvmeibt_praid	*_praid = (_praid_);												\
	if (_praid && !XDLIST_NULL(&(_praid->praid_topo_recalc_link))) {							\
		N_Tf(name, "praid=@UUID_LE CLEAR_TOPO_RECALC_REQUIRED", nvmeibt_praid_UUID(_praid));	\
		XDLIST_DEL(&(_praid->praid_topo_recalc_link));											\
	}																							\
} while (0)

BOOL nvmeibt_praid_applied_is_qualify_for_sync_stale(struct nvmeibt_praid *praid);
BOOL nvmeibt_praid_applied_is_qualify_for_JGC(struct nvmeibt_praid *praid);
static inline bool nvmeibt_praid_is_using_reset_registrants_for_owner_change(struct nvmeibt_praid *praid)
{
	return (nvmeibt_praid_is_type_EC(praid) || !nvmeibt_praid_is_type_EC(praid));	// For now always return true
}
bool nvmeibt_praid_is_deprecated_in_config(const struct nvmeibt_praid *praid);
static inline struct nvmeibt_chunk *nvmeibt_praid_get_chunk(struct nvmeibt_praid *praid)
{
	return (praid ? praid->praid_mgmt.its_chunk : NULL);
}
struct nvmeibt_block_device *nvmeibt_praid_get_blkdev(struct nvmeibt_praid *praid);

enum NVMEIBT_CLIENT_TR_REASON nvmeibt_praid_applied_sync_cmd_reason(struct nvmeibt_praid *praid);
struct nvmeibt_praid *nvmeibt_praid_get_praid_by_id(const union nvmeib_uuid *praid_id);

static inline BOOL nvmeibt_praid_is_being_deleted(struct nvmeibt_praid *praid)
{
	return (!praid || nvmeibt_chunk_is_being_deleted(praid->praid_mgmt.its_chunk));
}

const char *nvmeibt_praid_get_blkdev_name(const struct nvmeibt_praid *praid);
void nvmeibt_praid_mark_conf_corrupted(struct nvmeibt_praid *praid);
int nvmeibt_praid_validate_praids_config(void);
int nvmeibt_praid_lot_calc_topo_for_clients(struct nvmeibt_praid_lot *praid_lot,
											struct tTopoOfPraid *out_topo_for_clients,
											u8 io_perms_all,
											bool do_update_and_print);
void nvmeibt_seg_active_generate_all_segs_topo_for_clients(void);
void nvmeibt_praid_leader_got_notification_of_clients_sync_completion(struct nvmeibt_praid *praid);
void nvmeibt_praid_leader_generate_JBOD_praid_conf(struct nvmeibt_praid *praid);
enum NVMEIBT_CSV_TYPE;
enum nvmeibt_add_rv nvmeibt_praid_upd_committed_topo(struct nvmeibt_praid_serialized_topo *praid_topo_ptr);
void nvmeibt_praid_reset_due_to_convert_to_leader(struct nvmeibt_praid *praid);
void nvmeibt_praid_init(struct nvmeibt_praid *praid);
BOOL nvmeibt_praid_applied_is_reserialization_required(struct nvmeibt_praid *praid);
void nvmeibt_praid_applied_clear_reserialization_required(struct nvmeibt_praid *praid);
BOOL nvmeibt_praid_leader_is_topo_regeneration_required(struct nvmeibt_praid *praid);
union io_perms_bitfield nvmeibt_praid_calc_io_perms(struct nvmeibt_praid *praid, bool is_during_cold_recovery);

void nvmeibt_praid_upd_registrants_sync_cmd(struct nvmeibt_praid_lot *praid_lot, enum PRAID_REGISTRANTS_SYNC_CMD registrants_sync_cmd);
int nvmeibt_praid_leader_calc_topo_main(struct nvmeibt_praid *praid);
enum nvmeibt_add_rv nvmeibt_praid_add(struct mm_praid_conf *conf,
									  struct nvmeibt_chunk *chunk,
									  struct mm_vol_conf *vol,
									  bool is_updating_leader,
									  struct nvmeibt_praid **praid_out,
									  int config_tag);
void nvmeibt_praid_update_committed_lot_config(struct mm_praid_conf *conf, struct mm_vol_conf *vol, struct nvmeibt_praid **praid_out);
int nvmeibt_praid_remove(struct nvmeibt_praid *praid);
void nvmeibt_praid_free_all_at_exit(void);
void nvmeibt_praid_trim_specific_praid(struct nvmeibt_praid *praid, uint8_t trim_flag);
void nvmeibt_praid_trim_unused_entries(int config_tag, uint8_t trim_flag);
int nvmeibt_praid_dump_praid_status_line(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid *praid, BOOL is_leader, bool is_full_info_needed);
int nvmeibt_praid_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_praid *praid);
void nvmeibt_praid_print_leader_wire_topo_with_segs(struct nvmeibt_praid_leader *praid_leader);
void nvmeibt_praid_mark_all_praid_segs_post_update_actions_required(struct nvmeibt_praid *praid);
void nvmeibt_praid_lot_duplicate_content(struct nvmeibt_praid_lot *praid_lot_dst, struct nvmeibt_praid_lot *praid_lot_src);
int8_t nvmeibt_praid_append_to_report_to_mgmt(struct nvmeibt_praid *praid, struct nvmeibt_Str *json_payload);
void nvmeibt_praid_mark_immediate_report_to_mgmt_required(struct nvmeibt_praid *praid);
void nvmeibt_praid_lot_upd_registrants_sync_cmd(struct nvmeibt_praid_lot *praid_lot, enum PRAID_REGISTRANTS_SYNC_CMD registrants_sync_cmd);
bool nvmeibt_praid_upd_calculated_lot_from_praid_mgmt(struct nvmeibt_praid *praid);
void nvmeibt_praid_leader_we_have_a_new_baseline(struct nvmeibt_praid *praid, struct nvmeibt_praid_lot *src_praid_lot);
int nvmeibt_praid_validate_replacement_segs(struct nvmeibt_praid *praid);

#endif	// #ifndef NVMEIBT_PRAID

