/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DISK_LOCAL_DMA_POOLS_H
#define NVMEIBC_DISK_LOCAL_DMA_POOLS_H

struct nvmeibc_disk;

struct nvmeib_pool_percpu_counts
{
	int in_use;
	int alloc_fail;
	int in_place_free;
	int diff_cpu_free;
	int free_works_executed;
};

enum nvmeib_dma_pool_type {
	NVMEIB_DMA_POOL_TYPE_PRPL = 0,
	NVMEIB_DMA_POOL_TYPE_DUMMY_MD = 1,
	NVMEIB_DMA_POOL_TYPE_RD_MD = 2,
	NVMEIB_DMA_POOL_TYPE_MAX =	3,
};

struct nvmeib_nvme_req_dma_pool_info {
	struct llist_node dma_pool_free_link;
	int dma_pool_cpu;
	void *vaddr;
	dma_addr_t dma_addr;
	enum nvmeib_dma_pool_type pool_type;
};

struct nvmeib_pool_local_free_llist {
	struct llist_head head;
	struct llist_node *last;
	u32 size;
	u64 jiffies_first;
};

struct nvmeib_dma_pool {
	struct dma_pool *pool;
};

/*
	This struct is per-cpu, and contains the pools for each type of DMA pool (PRPL, RD_MD, DUMMY_MD).
	
	- When a request is being freed on the same cpu as the one it was allocated on, it is freed immediately to the pool.

	- When a request is being freed on a different cpu than the one it was allocated on, it is added to the local_reqs_free_list of the target cpu
		(we have a list percpu-percpu, and it is crucial to boost the performance, as this way we help reduce the call for llist_add, and we batch it into one call for each target cpu when threshold is met).
		When the local_reqs_free_list of the target cpu is meeting the threshold (checked each addition),
		this whole local_reqs_free_list of that cpu is being added to the target cpu reqs_free_list, and a work (dma_pool_free_reqs_work) is being scheduled on that cpu to free those requests.

	- When we allocate a request, we check reqs_free_list_in_progress for requests to free (we free in that case up to nvmeibc_dma_pools_max_free_reqs_per_work).
		this is to prevent case where the free work is never being executed due to heavy I/O load.
		When reqs_free_list_in_progress is empty, we take what's in reqs_free_list and add it to reqs_free_list_in_progress.
*/
struct nvmeib_dma_percpu_pools {
	struct nvmeib_dma_pool pools[NVMEIB_DMA_POOL_TYPE_MAX];
	struct nvmeib_pool_percpu_counts pcpu_counts[NVMEIB_DMA_POOL_TYPE_MAX];

	struct llist_head reqs_free_list; //requests from other cpus are being added here
	struct llist_node *reqs_free_list_in_progress; //this cpu is freeing these, and keeps adding more from the upper one

	struct nvmeib_pool_local_free_llist *local_reqs_free_list; //array of them, for each target cpu
	struct work_struct dma_pool_free_reqs_work;
	int cpu;
};

void *nvmeibc_percpu_dma_pool_alloc(struct nvmeib_dma_percpu_pools __percpu *percpu_pools,
									gfp_t mem_flags, dma_addr_t *dma_addr,
									enum nvmeib_dma_pool_type pool_type, struct nvmeibc_disk *disk);
void nvmeibc_percpu_dma_pool_free(struct nvmeib_dma_percpu_pools __percpu *percpu_pools,
									void *vaddr, dma_addr_t dma_addr,
									enum nvmeib_dma_pool_type pool_type,
									int submit_cpu);

int alloc_percpu_dma_pools(struct nvmeibc_disk *disk);
void free_percpu_pools(struct nvmeibc_disk *disk, struct nvmeib_dma_percpu_pools __percpu *percpu_pools);

int alloc_local_io_prpl_pool(struct nvmeibc_disk *disk);
int alloc_local_io_rd_md_pool(struct nvmeibc_disk *disk);
int alloc_local_io_md_dma_pool(struct nvmeibc_disk *disk);

#endif