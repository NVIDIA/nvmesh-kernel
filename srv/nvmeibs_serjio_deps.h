/**
 * manages disk locks fuctionality
 *
 * 5/2015
 *  */

#ifndef NVMEIBS_SERJIO_DEPS_H
#define NVMEIBS_SERJIO_DEPS_H

#include "common/kr_incs.h"
#include "nvmeibs_serjio_defs.h"
#include "nvmeib.h"
#include "nvmeibs_types.h"
#include "nvmeibs_serjio_stats.h"

struct nvmeibs_dev;
struct nmveibs_nvme_req;
struct nvmeibs_disk_info;
struct nvmeibs_disk_private_data;
struct nvmeib_alloc_n_map;
struct nvmeibs_serjio_disk_private_data;


/*
 * In order to integrate serjio code almost "as-is" into the simulator
 * I need to build an abstraction over nvmeibs_disk_info struct.
 */
int nvmeibs_disk_info_get_block_shift(struct nvmeibs_disk_info const * const di);

bool nvmeibs_disk_info_is_ready_for_serjio(struct nvmeibs_disk_info const * const di);

char const * nvmeibs_disk_info_get_disk_id(struct nvmeibs_disk_info const * const di);

struct nvmeibs_serjio_disk_private_data*
nvmeibs_disk_info_get_serjio_private_data(struct nvmeibs_disk_info const * const di);

void nvmeibs_disk_info_set_serjio_private_data(struct nvmeibs_disk_info *di,
	struct nvmeibs_serjio_disk_private_data* serjio_pd);

bool nvmeibs_disk_info_has_gpt(struct nvmeibs_disk_info const * const di);
void nvmeibs_disk_info_set_gpt_status(struct nvmeibs_disk_info *di, bool gpt_presents);

int nvmeibs_disk_info_get_md_size(struct nvmeibs_disk_info const * const di);
struct device * nvmeibs_disk_info_get_nvme_dma_device(struct nvmeibs_disk_info *di);
bool nvmeibs_disk_info_has_mtdt_extd(struct nvmeibs_disk_info const * const di);
u64 nvmeibs_disk_info_get_num_blks(struct nvmeibs_disk_info const * const di, bool hw_blocks);

u32 nvmeibs_serjio_map_on_dev(struct nvmeibs_dev *nic,
	struct nvmeib_alloc_n_map *mem);

int nvmeibs_serjio_unmapn_n_free(struct nvmeib_alloc_n_map *mem);

void nvmeibs_serjio_prepare_dma_region_for_device(struct nvmeibs_dev *nic_dev,
	u64 address, size_t size, enum dma_data_direction);

void nvmeibs_serjio_prepare_dma_region_for_cpu(struct nvmeibs_dev *nic_dev,
	u64 address, size_t size, enum dma_data_direction);

int nvmeibs_serjio_send_journal_abnd_free(struct nvmeibs_disk_info *di, u64 client_id,
					   u32 rng_num, binje_t rng_binje, u64 rng_gen_id, unsigned long *abnd_free_bitmap,
					   struct nvmeib_jrnl_ent_md *ent_md);

/*
 * serjio saves some data on the disks
 * in order to do this, it submits req and waits for completion
 * the request describe what changes should be done and where.
 */
int nvmeibs_serjio_update_disk(struct nvmeibs_disk_info *di,
		 struct nvmeibs_nvme_req *req);

/*
 * redirected calls to
 *    nvmeibs_disk_get_disks
 *    nvmeibs_disk_put_disks
 */
struct list_head* nvmeibs_serjio_get_disks(int* n_disks);
void nvmeibs_serjio_put_disks(void);

struct workq_struct *nvmeibs_serjio_wq_create(void);
void nvmeibs_serjio_wq_destroy(struct workq_struct *);

void nvmeibs_serjio_uuid_generate(u8 uuid[16]);

const char *nvmeibs_serjio_uuid_to_str(char *buf, const u8 uuid[16]);
const char *nvmeibs_serjio_uuid_le_to_str(char *buf, const u8 uuid[16]);


int nvmeibs_serjio_request_jgc(struct nvmeibs_disk_info *di, const char* seg_id);

/*
 * redirected calls to
 * 	nvmeibs_get_devices()
 * 	nvmeibs_put_devices()
 */
struct list_head *nvmeibs_serjio_get_devices(int *size);
void nvmeibs_serjio_put_devices(void);



#endif //NVMEIBS_SERJIO_DEPS_H
