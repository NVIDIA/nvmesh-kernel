/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibs_serjio_deps.h"
#include "nvmeibs_serjio_gpt.h"
#include "nvmeibs_main_sim.h"
#include "../toma/nvmeibr_disk_metadata_sim.h"

u32 nvmeibs_serjio_map_on_dev(struct nvmeibs_dev *nic, struct nvmeib_alloc_n_map *mem) {
	(void)nic; (void)mem; return 0;
}

int nvmeibs_serjio_unmapn_n_free(struct nvmeib_alloc_n_map *mem) {
	(void)mem; return 0;
}

void nvmeibs_serjio_prepare_dma_region_for_device(struct nvmeibs_dev *nic_dev, u64 address, size_t size, enum dma_data_direction dma_dir) {
	(void)nic_dev; (void)address; (void)size; (void)dma_dir;
}

struct device *nvmeibs_disk_info_get_nvme_dma_device(struct nvmeibs_disk_info * di){
	(void)di; return (void*)0x3ULL;
}

int nvmeibs_serjio_send_journal_abnd_free(struct nvmeibs_disk_info *di, u64 client_id,
					   u32 rng_num, binje_t rng_binje, u64 rng_gen_id, unsigned long *abnd_free_bitmap, struct nvmeib_jrnl_ent_md *ent_md){
	struct serverSimulator* srv = as_serverSimulator(di);
	u32 bmp[2] = {*(u32*)abnd_free_bitmap, 0};
	nvmeibs_nordda_process_jmd_free_abandoned(srv, client_id, rng_num, rng_binje, rng_gen_id, bmp, ent_md);

	return 0;
}

int nvmeibs_serjio_update_disk(struct nvmeibs_disk_info *di, struct nvmeibs_nvme_req *req){
	struct serverSimulator *srv = as_serverSimulator(di);
	struct ramDiskSimulator * D = &srv->ramDisk;
	bool const is_gpt_related = req->use_hw_blocks && req->disk_block < 3;
	u8 *data_ptr = is_gpt_related ? NULL : &D->mem[ COMMITTED_ADDR_AS(D, req->disk_block, SECTOR, BYTE)];
	u8 *md_data_ptr = is_gpt_related ? NULL : ramDiskSimulator_get_metadataptr_unsafe(D, req->disk_block);
	struct scatterlist *sgl;
	unsigned int sgl_nents;
	struct sg_table local_sgt = {};
	bool free_local_sgt = false;

	/* Not supported: buf_offset != 0 */
	BUG_ON(req->buf_offset != 0);

	if (req->use_sg) {
		sgl = req->table.sgl;
		sgl_nents = req->table.nents;
	} else {
		unsigned int n_pages = max_t(unsigned int, 1,
					     DIV_ROUND_UP(req->data_len, PAGE_SIZE));
		unsigned int i;

		if (sg_alloc_table(&local_sgt, n_pages, GFP_KERNEL))
			return -ENOMEM;
		for (i = 0; i < n_pages; i++) {
			size_t len = min_t(size_t, PAGE_SIZE,
					   req->data_len - (size_t)i * PAGE_SIZE);

			sg_set_buf(&local_sgt.sgl[i], (void *)req->buf_addrs[i], len);
		}
		sgl = local_sgt.sgl;
		sgl_nents = n_pages;
		free_local_sgt = true;
	}

	// --------------------------------- R/W paritions in units of hardware sectors
	if (req->use_hw_blocks && req->disk_block < 3) {
		BUG_ON(req->nvme_op != nvme_cmd_read);							// Only Toma can write GPT, serjio only reads!
		BUG_ON(req->buf_offset != 0);
		if (req->disk_block == 0) {
			BUG();														// Serjio should never access MBR
		} else if (req->disk_block == GPT_PRIMARY_HDR_LBA) {            // GPT read
			struct gpt_header gpt_hdr;

			BUG_ON(req->data_len > (1 << NVMEIBC_SECTOR_SHIFT));
			nvmeibr_disk_metadata_store_gpt(srv, &gpt_hdr, true);
			BUG_ON(sg_copy_from_buffer(sgl, sgl_nents, &gpt_hdr, sizeof(gpt_hdr)) != sizeof(gpt_hdr));
		} else if (req->disk_block == GPT_PRIMARY_ENTS_START_LBA) {		// GPT entries read
			unsigned int max_gpt_ent = nvmeibr_disk_metadata_num_gpt_entries(srv);
			unsigned int sg_idx;
			struct scatterlist *sg;
			unsigned int start_ent = 0;

			BUG_ON((req->data_len % sizeof(struct gpt_entry)) != 0);

			for_each_sg(sgl, sg, sgl_nents, sg_idx) {
				unsigned int seg_len = sg->length;
				struct gpt_entry *dst = sg_virt(sg);
				unsigned int n_slice;

				BUG_ON((seg_len % sizeof(struct gpt_entry)) != 0);

				if (start_ent < max_gpt_ent) {
					n_slice = seg_len / (unsigned int)sizeof(struct gpt_entry);
					n_slice = min(n_slice, max_gpt_ent - start_ent);
					nvmeibr_disk_metadata_store_entries(srv, dst, start_ent, n_slice, max_gpt_ent, NULL);
					start_ent += n_slice;
				} else {
					memset(dst, 0, seg_len);
				}
			}
			BUG_ON(start_ent != max_gpt_ent);
		}
		goto _out;
	}

	// --------------------------------- R/W of serjio-db or journal partitions in units of software sectors
	if (req->nvme_op != nvme_cmd_write_zeroes) {
		BUG_ON((req->data_len % (1 << NVMEIBC_SECTOR_SHIFT)) != 0);
		BUG_ON((req->data_len % 4096) != 0);						// Full-4Ks
	}
	BUG_ON(req->buf_offset != 0);
	switch (req->nvme_op) {
		case nvme_cmd_read:{
			memcpy(req->metadata, md_data_ptr, req->mtdt_size);
			BUG_ON(sg_copy_from_buffer(sgl, sgl_nents, data_ptr, req->data_len) != req->data_len);
			break;
		}
		case nvme_cmd_write:{
			memcpy(md_data_ptr, req->metadata, req->mtdt_size);
			BUG_ON(sg_copy_to_buffer(sgl, sgl_nents, data_ptr, req->data_len) != req->data_len);
			break;
		}
		case nvme_cmd_write_zeroes: {
			u8 zeroed_md[DISK_MAX_MD_SIZE_BYTE] = {[0 ... DISK_MAX_MD_SIZE_BYTE-1] = 0};
			memset(data_ptr, 0, req->data_len);
			ramDiskSimulator_wipeMDRange(D, req->disk_block, req->data_len >> NVMEIBC_SECTOR_SHIFT, zeroed_md);
			break;
		}
		default:{
			WARN(true, "unhandled nvme command\n");
		}
	}
_out:
	if (free_local_sgt)
		sg_free_table(&local_sgt);
	req->cb(req->arg,/*status*/0,/*result, unused*/0);
	return 0;
}

struct list_head* nvmeibs_serjio_get_disks(int* n_disks) {
	(void)n_disks;
	return NULL;
}
void nvmeibs_serjio_put_disks(void){}

static LIST_HEAD(devices_list);
struct list_head *nvmeibs_serjio_get_devices(int *size) {
	(void)size;
	return &devices_list;
}
void nvmeibs_serjio_put_devices(){}

struct workq_struct *serjios_wq;	// All serjios run on the same wq for easier draining and debug
static int n_wq_refs;				// Daniel: Todo, make atomic?
struct workq_struct *nvmeibs_serjio_wq_create(void) {
	if (!serjios_wq)
		serjios_wq = wq_create("s_serjio_wq");
	n_wq_refs++;
	return serjios_wq;
}

void nvmeibs_serjio_wq_destroy(struct workq_struct *io_wq){
	n_wq_refs--;
	if (n_wq_refs == 0) {
		wq_destroy(io_wq /* == serjios_wq*/);
		serjios_wq = NULL;
	}
}

u16 nvmeibs_nvme_get_vendor(const struct nvmeibs_disk_info *info){
	return ((u64)info&0xFFFF);		// Psudo random value ?
}

void nvmeibs_serjio_uuid_generate(u8 uuid[16])
{
	uuid_generate_random(uuid);
}

const char *nvmeibs_serjio_uuid_to_str(char *buf, const u8 uuid[16])
{
	uuid_unparse(uuid, buf);
	return buf;
}

const char *nvmeibs_serjio_uuid_le_to_str(char *buf, const u8 uuid[16]) {
	uuid_unparse(uuid, buf);
	return buf;
}

/******************* Simulator of nvmeibs_toma.c ******************************/

int nvmeibs_serjio_request_jgc(struct nvmeibs_disk_info *di, const char* seg_id){
	(void)di;
	return tomaSimulator_JGC_launch(seg_id);
}

struct nvmeibs_um_comm;
struct nvmeibs_um_comm *nvmeibs_get_um_comm(void) {
	return (void*)-1; /* Return an invalid value, should never be used in simulator */
}

void nvmeibs_um_comm_serjio_state_changed(
    struct nvmeibs_um_comm *p, const char *disk_id, u16 vendor_id,
    char *model_str, enum nvmeibs_serjio_status serjio_status) {
	(void)p;
	(void)disk_id;
	(void)vendor_id;
	(void)model_str;
	(void)serjio_status;
}
