/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * manages disk locks fuctionality
 *
 * 5/2015
 *  */


#ifndef NVMEIBS_SERJIO_H
#define NVMEIBS_SERJIO_H
#include "common/kr_incs.h"
#include "nvmeib.h"
#include "nvmeibs_types.h"
#include "nvmeibs_msgs_shared.h"

struct nvmeibs_disk_info;
struct nvmeibs_dev;

struct nvmeibs_disk_private_data;
struct nvmeib_alloc_n_map;
struct volume_server_gen_rsp_uuid_jour;

/*
 * Roman
 * allocates/reconnects client to its journal entries
 */
int nvmeibs_serjio_alloc_journal_range(struct nvmeibs_disk_info* di, u32 client_id,
				       uuid_be client_uuid, const char *client_host,
					   const binje_t binje_req, const struct nvmeib_jrange_cache *jrc,
					   struct nvmeib_jrange_rsp *rsp);

/*
 * Roman; the client is going to disapear, let it to return the range it uses
 */
int nvmeibs_serjio_return_journal_range(struct nvmeibs_disk_info* di, u32 cl_jrnl_rng);

/*
 * Roman
 * connects between lock channel and disks?
 */
int nvmeibs_serjio_map_all_jmdc_for_nic (struct nvmeibs_dev *nic);
int nvmeibs_serjio_map_disk_jmd_cache(struct nvmeibs_disk_info *di, struct nvmeibs_dev *nic);

/* Roman:
 * checks and sets (if needed) serjio data for the disk
 */
int nvmeibs_serjio_set_journal_info(struct nvmeibs_disk_info* di, struct nvmeibs_toma_journal_msg* journal_msg);

/* Roman
 * initializes serjio data on disk in the private area
 */
int nvmeibs_serjio_disk_init(struct nvmeibs_disk_info *di);

void nvmeibs_serjio_j2d_unmap_all(struct nvmeibs_disk_private_data *disk_pd);
void nvmeibs_serjio_unmap_jmd_cache_on_dev(struct nvmeibs_dev *nic);
void nvmeibs_serjio_disk_free(struct nvmeibs_disk_info *di);
enum nvmeibs_serjio_status nvmeibs_serjio_get_status(struct nvmeibs_disk_info *di);
int nvmeibs_serjio_jmdc_entry_set(void *jrange_handle, u64 rng_gen_id,
								  int n_ent, u16 *ent_idx,
								  const struct nvmeib_jrnl_ent_md *ent_md,
								  const union jblock_md *ent_data,
								  bool rdma_sync);
void *nvmeibs_serjio_get_jrange_handle(struct nvmeibs_disk_info *di, u32 cid,
									   u32 jrnl_rng, struct nvmeibs_dev *dev,
									   void **jmdc_map_handle);
void nvmeibs_serjio_put_jrange_handle(void *rng_handle, void *jmdc_map_handle);
int nvmeibs_serjio_get_jmdc_dev_map(struct nvmeibs_disk_info *di,
				struct nvmeibs_dev *dev,
				void **jmdc_map_handle);
void nvmeibs_serjio_put_jmdc_dev_map(void *jmdc_map_handle);
int nvmeibs_serjio_get_jrange_jmdc_rai(void *rng_handle, void *jmdc_map_handle, struct nvmeib_remote_access_info *jmdc_rai);
int nvmeibs_serjio_blkset_recovered(struct nvmeibs_disk_info *di, uuid_be client_uuid,
									u32 range_idx, u32 entry_idx, u64 blkset_slba, u64 gen_id);
int nvmeibs_serjio_get_client_uuid_rng(struct nvmeibs_disk_info *di, uuid_be client_uuid, binje_t binje);

#define DECLARE_SERJIO_RNG_CB_GET_JMDC_ENT_FN(fn) \
	union jblock_md * fn(void *rng_handle, unsigned rng_entry)

typedef DECLARE_SERJIO_RNG_CB_GET_JMDC_ENT_FN((*nvmesh_serjio_rng_cb_get_jmdc_ent_fn));

#define DECLARE_SERJIO_RNG_CB_FN(fn) \
	int fn(u32 range_idx, binje_t range_binje, unsigned num_entries,\
	       uuid_be client_uuid,\
	       u64 start_clsect, u32 size_clsect, u64 gen_id,\
	       int n_dirty_ents_in_seg,\
	       const unsigned long *dirty_ents_in_seg_bmp,\
	       int n_abnd_ents_in_seg,\
	       const unsigned long *abnd_ents_in_seg_bmp,\
	       void *rng_handle, nvmesh_serjio_rng_cb_get_jmdc_ent_fn get_jmdc_ent_fn,\
	       const struct nvmeib_jrnl_ent_md *ent_md, size_t ent_md_sz, void *ctx)

typedef DECLARE_SERJIO_RNG_CB_FN((*nvmeibs_serjio_rng_cb_fn));

int nvmeibs_serjio_call_for_each_assigned_range(
	struct nvmeibs_disk_info *di, u32 start_range_idx, u32 num_ranges,
	const char *seg_uuid, nvmeibs_serjio_rng_cb_fn cb_fn, void *ctx);

int nvmeibs_serjio_call_for_client_range(
	struct nvmeibs_disk_info *di, uuid_be client_uuid, binje_t binje,
	const char *seg_uuid, nvmeibs_serjio_rng_cb_fn cb_fn, void *ctx);

int nvmeibs_serjio_get_jrnl_clsects(struct nvmeibs_disk_info *di,
								 u64 *jrnl_start_clsect, u64 *jrnl_len_clsect);

/* Called via no-RDDA Gen Command at the end of any recovery process to free journal entries */
int nvmeibs_serjio_free_jrnl_ents(struct nvmeibs_disk_info *di, const char *serjio_boot_id,
								  const char *seg_uuid, enum nvmeib_recov_src recov_src, u64 blkset_slba,
								  int num_ents, const struct nvmeib_free_ents_data *free_ents);

/* Called via the LOSER payload that is appended to TOMA UNREGISTER msg -
 * Used to abandon entries that were used for an IO that failed */
int nvmeibs_serjio_abnd_jrnl_ents(struct nvmeibs_disk_info *di, u32 cl_jrnl_rng,
								  unsigned long *abnd_ents_bmp, u8 *ents_gen_id);

int nvmeibs_serjio_clean_journal_for_disk_range(
	struct nvmeibs_disk_info *di, const char *seg_uuid_str, u64 disk_range_start_lba,
	u64 disk_range_end_lba, bool seg_delete);

const char *nvmeibs_serjio_get_boot_id(struct nvmeibs_disk_info *di);

int nvmeibs_serjio_gpt_update(struct nvmeibs_disk_info *di, bool primary,
							  enum nvmeib_main_gpt_update_flags gpt_update, int user_pid,
							  const void __user *gpt_hdr, size_t gpt_hdr_sz,
							  const void __user *gpt_entries, size_t gpt_entries_sz);

int nvmeibs_serjio_gpt_update_done(struct nvmeibs_disk_info *di, bool primary,
				  enum nvmeib_main_gpt_update_flags gpt_update, int status);

/* Called via GEN command by JAM to erase journal ents */
int nvmeibs_serjio_erase_jam_ent(void *jrange_handle, const binje_t rng_binje, u64 rng_gen_id, int num_ents,
								 const struct nvmeib_gen_cmd_jam_ent_erase *erase_ents,
								 enum nvmeib_gen_cmd_jam_ent_erase_reason erase_reason);

int nvmeibs_serjio_get_seg_range(struct nvmeibs_disk_info *di, const char *seg_uuid_str, u64 *start_lba, u64 *end_lba);

int nvmeibs_toma_report_event_serjio_state_change(
    const char *disk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status state);

int  nvmeibs_serjio_jmd_decode_j2d_chain(const union jblock_md *jentry, const binje_t binje, u64 *j2d_start, u64 *j2d_end);

static inline u64 nvmeibs_serjio_jmd_decode_j2d_start(const union jblock_md *jentry, const binje_t binje)
{
	u64 j2d_start = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	nvmeibs_serjio_jmd_decode_j2d_chain(jentry, binje, &j2d_start, NULL);
	return j2d_start;
}

static inline u64 nvmeibs_serjio_jmd_decode_j2d_end(const union jblock_md *jentry, const binje_t binje)
{
	u64 j2d_end = NVMEIB_EC_INVALID_BLOCKSET_SLBA;
	nvmeibs_serjio_jmd_decode_j2d_chain(jentry, binje, NULL, &j2d_end);
	return j2d_end;
}

int nvmeibs_serjio_gpt_update_cancel(struct nvmeibs_disk_info *di);

ssize_t fill_serjios(void *dummy, char *buffer, size_t len);

#endif //NVMEIBS_SERJIO_H
