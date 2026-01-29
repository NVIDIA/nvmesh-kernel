/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * Implements API between serhio and nvmeibs_disk_info. 12/2018
 */

#include "nvmeibs_serjio_deps.h"
#include "ib_incs.h"
#include "rdma/ib_verbs.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_main.h"
#include "nvmeib_public.h"
#include "nvmeibs_client.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_trace.h"
#include <linux/device-mapper.h> //SECTOR_SHIFT
#include "nvmeibs_toma.h" //jgc

struct device *nvmeibs_disk_info_get_nvme_dma_device(struct nvmeibs_disk_info * di) {
	if (di->dev != NULL)
		return get_nvme_dma_device(di->dev);
#if KS_KREF_USES_REFCOUNT
	else if (di->gendisk != NULL && di->gendisk->queue != NULL && gendisk_to_bdi(di->gendisk) != NULL)
		return gendisk_to_device(di->gendisk);
#else
	else if (di->gendisk != NULL && di->gendisk->queue != NULL)
		return gendisk_to_device(di->gendisk);
#endif
	return NULL;
}

u32 nvmeibs_serjio_map_on_dev(struct nvmeibs_dev *nic,
	struct nvmeib_alloc_n_map *mem)
{
	u32 ret_rkey = 0;

	NFIN;

	mem->pd = nic->dev->pd;
	mem->access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;
	mem->ioaddr = 0; /*we are using the entire memory*/
	mem->dma_dir = DMA_BIDIRECTIONAL;
	if (nvmeib_mem_alloc_n_map(mem) == 0) {
		ret_rkey = mem->rkey;
		_NT(trace_serjio_deps_nvmeibs_serjio_map_on_dev, "ret_rkey=@RET_RKEY, pd=@PD, access=@ACCESS, device_name=@DEVICE_NAME",
			mem->rkey, mem->pd, mem->access_flags,
			mem->pd->device->name);
	}
	else
		_NE(error_serjio_deps_nvmeibs_serjio_map_on_dev, "memory map allocation failed");

	NFOUT;
	return ret_rkey;
}

int nvmeibs_serjio_unmapn_n_free(struct nvmeib_alloc_n_map *mem){
	return nvmeib_mem_unmapn_n_free(mem);
}

void nvmeibs_serjio_prepare_dma_region_for_device(struct nvmeibs_dev *nic_dev,
	u64 address, size_t size, enum dma_data_direction dma_data_dir)
{
	ib_dma_sync_single_for_device(N2IB(nic_dev), address, size, dma_data_dir);
}

void nvmeibs_serjio_prepare_dma_region_for_cpu(struct nvmeibs_dev *nic_dev,
	u64 address, size_t size, enum dma_data_direction dma_data_dir)
{
	ib_dma_sync_single_for_cpu(N2IB(nic_dev), address, size, dma_data_dir);
}


int nvmeibs_serjio_send_journal_abnd_free(struct nvmeibs_disk_info *di, u64 client_id,
	u32 rng_num, binje_t rng_binje, u64 rng_gen_id, unsigned long *abnd_free_bitmap, struct nvmeib_jrnl_ent_md *ent_md)
{
	const char* disk_name = nvmeibs_disk_info_get_disk_id(di);
	return nvmeibs_client_send_journal_abnd_free(client_id, disk_name, rng_num, rng_gen_id,
			abnd_free_bitmap, ent_md, rng_binje);
}

int nvmeibs_serjio_update_disk(struct nvmeibs_disk_info *di,
		 struct nvmeibs_nvme_req *req){
	return submit_local_cmd(di, req);
}


struct list_head* nvmeibs_serjio_get_disks(int* n_disks){
	return nvmeibs_disk_get_disks(n_disks);
}

void nvmeibs_serjio_put_disks(void){
	nvmeibs_disk_put_disks();
}

struct workq_struct *nvmeibs_serjio_wq_create(void)
{
	return wq_create(proc_name_format("S", "WQ", "serjio"));
}

void nvmeibs_serjio_wq_destroy(struct workq_struct *io_wq)
{
	wq_destroy(io_wq);
}

void nvmeibs_serjio_uuid_generate(u8 uuid[16])
{
	generate_random_uuid(uuid);
}

const char *nvmeibs_serjio_uuid_to_str(char *buf, const u8 uuid[16])
{
	sprintf(buf, "%pUb", uuid);
	return buf;
}

const char *nvmeibs_serjio_uuid_le_to_str(char *buf, const u8 uuid[16])
{
	sprintf(buf, "%pUl", uuid);
	return buf;
}

int nvmeibs_serjio_request_jgc(struct nvmeibs_disk_info *di, const char* seg_id){
	(void)seg_id;
	return nvmeibs_toma_report_event_serjio_request_jgc(seg_id, di->disk_id);
}

struct list_head *nvmeibs_serjio_get_devices(int *size)
{
	return nvmeibs_get_devices(size);
}

void nvmeibs_serjio_put_devices(void)
{
	nvmeibs_put_devices();
}
