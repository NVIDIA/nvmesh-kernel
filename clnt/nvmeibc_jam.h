#ifndef NVMEIBC_JAM_H
#define NVMEIBC_JAM_H
/* This file is an interim layer between block layer and pausable layer.
 * It supplies transactional API (journal management for trasnactional disk
 * operations) by Erasure Coded Volumes. */

#include "../core_unitest/corecomm_injections.h"
#include "nvmeibs_msgs_shared.h"

enum nvmeibc_jam_jidx_event {
	NVMEIBC_JIDX_EVT_END_USE = 0xE0,
	NVMEIBC_JIDX_EVT_ERASE,
	NVMEIBC_JIDX_EVT_ERASE_COMP,
	NVMEIBC_JIDX_EVT_ERASE_COMP_ERR,
	NVMEIBC_JIDX_EVT_ABANDON,
	NVMEIBC_JIDX_EVT_FREE_ABND,
};

/* Consider WD timeout and #disks in full-slice */
struct nvmeibc_cinst_params_core;
void* nvmeibc_jam_init(     const struct nvmeibc_cinst_params_core *p);
void  nvmeibc_jam_exit(     const struct nvmeibc_cinst_params_core *p);
int nvmeibc_jam_fill_status(const struct nvmeibc_cinst_params_core *p, char* buf, int len);

int nvmeibc_jam_disk_add(struct nvmeibc_disk *disk,
						 unsigned long *free_ents_bmp,
						 union jblock_md *jmdc_tbl,
						 struct nvmeib_jrnl_ent_md *ent_md);
void nvmeibc_jam_disk_del(struct nvmeibc_disk *disk);

/* @wait_bound_abnd - when false, fail allocation with -EDEADLK
 *                    if any entry to be bound is ABANDONED. */
struct nvmeib_cpu_mask_info;
int nvmeibc_jam_lbas_alloc(int n_disks, struct nvmeibc_disk *disks[], u32 txid,
	u64 dlbas[], u64 res_jlbas[], bool wait_bound_abnd, const struct nvmeib_cpu_mask_info *cpu_mask_info, unsigned long timeout_jiffies, void *ctx);

/* @wr_sts_bm - bit i is O if journal-write of jlba=jlbas[i]
 *  		    (on disk=disks[i]) was issued && completed OK.
 *
 * This can be extended from bitmap to 3 values per entry so
 * in case journal-write was not issued, we wont do remote-erase */
void nvmeibc_jam_lbas_free(int n_disks, struct nvmeibc_disk *disks[], u64 jlbas[], u32 wr_sts_bm);
int  nvmeibc_jam_abandon_lba(struct nvmeibc_disk *disk, u64 jlba, u8 *gen_id);

struct jentry_md;
int nvmeibc_jam_jmd_set(struct nvmeibc_disk *disk, u64 lba,
						struct jentry_md *jentry,
						int *ret_idx, bool *lba_enc, u8 *ret_ent_gen_id);

struct nvmeibc_disk_gen_cmd;
int nvmeibc_jam_jentry_erase_comp(struct nvmeibc_disk_gen_cmd *gen_cmd);
int nvmeibc_jam_process_recv_comp(struct nvmeibc_disk *disk,
				  struct volume_server_req *req);
size_t nvmeibc_jam_fill_disk_status(struct nvmeibc_disk *disk, char *buf, size_t len);
u64 nvmeibc_jam_decode_lba(u64 enc_lba);

void nvmeibc_jam_on_periodic(struct nvmeibc_disk *disk);

int nvmeibc_jam_lba_2_idx(struct nvmeibc_disk *disk, u64 lba);
corecomm_inj_code(
	int nvmeibc_jam_lba_2_idx(struct nvmeibc_disk *disk, u64 lba);
);


struct volume_server_cmd_jmd_free_abnd;

struct abnd2free {
	struct volume_server_cmd_jmd_free_abnd jreq;
	u64 hdr_tag;
};

struct abnd_free_decode_ctx {
	struct nvmeibc_disk *disk;
	u32 rng_num;
	u64 rng_gen_id;
	int abnd_free_idx_arr[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	u8 free_idx_gen_id[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	int abnd_free_idx_n;
	binje_t binje;
};
struct nvmeibc_ib_admin_channel;
int process_jmd_free_abandoned(struct nvmeibc_disk *disk,
			       struct nvmeibc_ib_admin_channel *ch,
			       struct abnd2free *a2f);

int process_jmd_free_abnd_decoded(struct nvmeibc_disk *disk,
			       struct abnd_free_decode_ctx *decode_ctx);

void nvmeibc_jam_disk_cache_init(struct nvmeibc_disk *disk);
struct volume_client_config_jrange_cache;
void nvmeibc_jam_disk_cache_hton(struct nvmeibc_disk *disk,
	struct volume_client_config_jrange_cache *clnt_jrc);
struct nvmeib_jrange_cache;
void nvmeibc_jam_disk_cache_local(struct nvmeibc_disk *disk,
	struct nvmeib_jrange_cache *clnt_jrc);

#define NUM_JENTS_JAM_USES_IN_JRI(disk)	((int)((disk)->jour.n_ents))	// Dont use: NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE due to existance of 'qa_ec_stress_debug' EC-1480

/********************** Debug API: Used for unitesting ************************/
#if defined(BLKDEV_SIMULATOR)
struct jam_simu_stats {
	u64 n_bound_to_transient;
	u64 n_pending_timeout;
	u64 n_pending_on_empty;
};

/* Peek: Which block will be allocated to next io to this disk:
   -1 if illegal, [0..journal_size) otherwise*/
int  nvmeibc_jam_simu_alloc_entry(const struct nvmeibc_disk *disk, u64 dlba, u32 txid, bool dry_run);
void nvmeibc_jam_simu_process_entry_event(const struct nvmeibc_disk *disk, int entry, enum nvmeibc_jam_jidx_event event);

struct jam_simu_stats nvmeibc_get_jam_simu_stats(const struct nvmeibc_disk *disk);
void nvmeibc_jam_simu_inject_hash_function(const struct nvmeibc_disk *disk, u64 (*hash64)(u64, unsigned int));

static inline void nvmeibc_jam_simu_free_entry(struct nvmeibc_disk *disk, int entry){
	nvmeibc_jam_simu_process_entry_event(disk, entry, NVMEIBC_JIDX_EVT_END_USE);
}

/* Verify that serjio indeed cleaned the abonded of above */
void nvmeibc_jam_simu_verify_cleand(      struct nvmeibc_disk *disk, u32 e);
bool nvmeibc_jam_simu_has_abandoned(struct nvmeibc_disk *disk);
void nvmeibc_jam_drain_disk_ops_block_idle_unsafe(struct nvmeibc_disk *disk);	// Drain all in air ops (including reset) of JAM. Can be used only when block layer is inactive
#endif

#endif /* NVMEIBC_JAM_H */

