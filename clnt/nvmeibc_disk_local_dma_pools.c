#include "nvmeibc_disk_local_dma_pools.h"

#include "nvmeibc_disk.h"

int nvmeibc_dma_pools_max_free_list_reqs = 32;
module_param_named(dma_pools_max_free_list_reqs, nvmeibc_dma_pools_max_free_list_reqs, int, 0644);
MODULE_PARM_DESC(dma_pools_max_free_list_reqs, "Maximum number of requests to keep in the free list");

int nvmeibc_dma_pools_max_jiffies_free_list = 5;
module_param_named(dma_pools_max_jiffies_free_list, nvmeibc_dma_pools_max_jiffies_free_list, int, 0644);
MODULE_PARM_DESC(dma_pools_max_jiffies_free_list, "Maximum number of jiffies to keep requests in the free list");

uint nvmeibc_dma_pools_max_free_reqs_per_work = 16;
module_param_named(dma_pools_max_free_reqs_per_work, nvmeibc_dma_pools_max_free_reqs_per_work, uint, 0644);
MODULE_PARM_DESC(dma_pools_max_free_reqs_per_work, "Maximum number of requests to free per work");

static void nvmeibc_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma_addr)
{
	BUG_ON(pool == NULL);
	dma_pool_free(pool, vaddr, dma_addr);
}

/*
	notes about bound:
	- if true, we will bound the number of requests to free per work
		user must use local_irq_save/restore when calling the function (loop will be short so it wont be a problem, as we are bound)
	- if not bound, we want to avoid disabled interrupts until the while loop is done (as it might be big),
		so we use local_irq_save/restore to protect only struct llist_node *node from being
		altered while we are freeing requests in the other context, and nullify reqs_free_list_in_progress.
*/
static void __free_llist_nodes(struct nvmeib_dma_percpu_pools *this_cpu_pools_struct, bool bound)
{
	u32 n_freed = 0;
	unsigned long flags = 0;
	struct llist_node *node = NULL;
	u32 max_free_reqs_per_work = bound ? nvmeibc_dma_pools_max_free_reqs_per_work : UINT_MAX;

	if (!bound) {
		local_irq_save(flags);
	}

	if (!this_cpu_pools_struct->reqs_free_list_in_progress) {
		this_cpu_pools_struct->reqs_free_list_in_progress = llist_del_all(&this_cpu_pools_struct->reqs_free_list);
	}

	node = this_cpu_pools_struct->reqs_free_list_in_progress;
	
	if (!bound) {
		this_cpu_pools_struct->reqs_free_list_in_progress = NULL;
		local_irq_restore(flags);
	}

cont:
	while (node && n_freed < max_free_reqs_per_work) {
		struct nvmeib_nvme_req_dma_pool_info *req_pool_info = container_of(node, struct nvmeib_nvme_req_dma_pool_info, dma_pool_free_link);
		enum nvmeib_dma_pool_type pool_type = req_pool_info->pool_type;
		BUG_ON(req_pool_info->dma_pool_cpu != this_cpu_pools_struct->cpu);
		node = llist_next(node);
		nvmeibc_dma_pool_free(this_cpu_pools_struct->pools[pool_type].pool, req_pool_info->vaddr, req_pool_info->dma_addr);
		n_freed++;
	}

	if (bound) {
		/* not needed otherwise as its was nullified */
		this_cpu_pools_struct->reqs_free_list_in_progress = node;
	} else if ((node = llist_del_all(&this_cpu_pools_struct->reqs_free_list))) {
		/* not bound and there's more reqs, continue loop */
		goto cont;
	}
}

void *nvmeibc_percpu_dma_pool_alloc(struct nvmeib_dma_percpu_pools __percpu *percpu_pools,
									gfp_t mem_flags, dma_addr_t *dma_addr,
									enum nvmeib_dma_pool_type pool_type, struct nvmeibc_disk *disk) {
	void *dma_block;
    int cpu = get_cpu();
	struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu);
	struct nvmeib_dma_pool *this_cpu_type_pool = &this_cpu_pools_struct->pools[pool_type];
	struct dma_pool *this_cpu_type_dma_pool = this_cpu_type_pool->pool;
	unsigned long flags;

	BUG_ON(percpu_pools == NULL);

	local_irq_save(flags);
    __free_llist_nodes(this_cpu_pools_struct, /*bound*/ true); // we want to bound the freeing while we are allocating
	local_irq_restore(flags);
	dma_block = dma_pool_alloc(this_cpu_type_dma_pool, mem_flags, dma_addr);
	if (dma_block) {
		this_cpu_pools_struct->pcpu_counts[pool_type].in_use++;
	} else {
		this_cpu_pools_struct->pcpu_counts[pool_type].alloc_fail++;
	}
    put_cpu();
    return dma_block;
}

inline static bool threshold_met_for_local_free_reqs_llist(struct nvmeib_pool_local_free_llist *this_cpu_llist_for_target_cpu)
{
	return (this_cpu_llist_for_target_cpu->size >= nvmeibc_dma_pools_max_free_list_reqs)
		|| ((jiffies - this_cpu_llist_for_target_cpu->jiffies_first) > nvmeibc_dma_pools_max_jiffies_free_list);
}

static void __local_free_reqs_llist_init(struct nvmeib_pool_local_free_llist *llist)
{
	init_llist_head(&llist->head);
	llist->last = NULL;
	llist->size = 0;
	llist->jiffies_first = 0;
}


static void __local_free_reqs_llist_add(struct nvmeib_pool_local_free_llist *this_cpu_llist_for_target_cpu,
										struct nvmeib_nvme_req_dma_pool_info *req_dma_pool_info,
										struct nvmeib_dma_percpu_pools *target_pools)
{
	struct llist_node *node;
	int target_cpu = req_dma_pool_info->dma_pool_cpu;
	BUG_ON(target_cpu != target_pools->cpu);
	if (this_cpu_llist_for_target_cpu->last == NULL) {
		// meaning this is the first node in the list
		this_cpu_llist_for_target_cpu->last = &req_dma_pool_info->dma_pool_free_link;
		this_cpu_llist_for_target_cpu->jiffies_first = jiffies;
	}

	__llist_add(&req_dma_pool_info->dma_pool_free_link, &this_cpu_llist_for_target_cpu->head);
	this_cpu_llist_for_target_cpu->size++;

	if (threshold_met_for_local_free_reqs_llist(this_cpu_llist_for_target_cpu)) {
		node = __llist_del_all(&this_cpu_llist_for_target_cpu->head);
		if (node) {
			nvmeib_public_llist_add_batch(node, this_cpu_llist_for_target_cpu->last, &target_pools->reqs_free_list);
			schedule_work_on(target_cpu, &target_pools->dma_pool_free_reqs_work);
			__local_free_reqs_llist_init(this_cpu_llist_for_target_cpu);
		}
	}
}

inline static void add_to_local_free_reqs_llist(struct nvmeib_pool_local_free_llist *local_llist_array,
												struct nvmeib_nvme_req_dma_pool_info *req_dma_pool_info,
												struct nvmeib_dma_percpu_pools __percpu *percpu_pools) 
{
	struct nvmeib_pool_local_free_llist *this_cpu_llist_for_target_cpu = &local_llist_array[req_dma_pool_info->dma_pool_cpu];
	struct nvmeib_dma_percpu_pools *target_pools = per_cpu_ptr(percpu_pools, req_dma_pool_info->dma_pool_cpu);
	unsigned long flags;
	local_irq_save(flags);
	__local_free_reqs_llist_add(this_cpu_llist_for_target_cpu, req_dma_pool_info, target_pools);
	local_irq_restore(flags);
}

static void schedule_dma_pool_free_work(struct nvmeib_dma_percpu_pools *this_cpu_pools_struct,
										struct nvmeib_nvme_req_dma_pool_info *req_dma_pool_info,
										struct nvmeib_dma_percpu_pools __percpu *percpu_pools) {
	
	add_to_local_free_reqs_llist(this_cpu_pools_struct->local_reqs_free_list, req_dma_pool_info, percpu_pools);
}

void nvmeibc_percpu_dma_pool_free(struct nvmeib_dma_percpu_pools __percpu *percpu_pools,
									void *vaddr, dma_addr_t dma_addr,
									enum nvmeib_dma_pool_type pool_type,
									int submit_cpu) {
	int cpu = get_cpu();
	struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu); // current cpu free was called at
	struct nvmeib_dma_pool *this_cpu_type_pool = &this_cpu_pools_struct->pools[pool_type];
	struct nvmeib_nvme_req_dma_pool_info *req_dma_pool_info = vaddr;
	BUG_ON(req_dma_pool_info == NULL);
	BUG_ON(pool_type >= NVMEIB_DMA_POOL_TYPE_MAX);
	BUG_ON(submit_cpu >= nr_cpu_ids);
	if (submit_cpu == cpu) {
		this_cpu_pools_struct->pcpu_counts[pool_type].in_place_free++;
		nvmeibc_dma_pool_free(this_cpu_type_pool->pool, vaddr, dma_addr);
	} else {
		this_cpu_pools_struct->pcpu_counts[pool_type].diff_cpu_free++;
		req_dma_pool_info->vaddr = vaddr;
		req_dma_pool_info->dma_addr = dma_addr;
		req_dma_pool_info->dma_pool_cpu = submit_cpu;
		req_dma_pool_info->pool_type = pool_type;
		req_dma_pool_info->dma_pool_free_link.next = NULL;
		schedule_dma_pool_free_work(this_cpu_pools_struct, req_dma_pool_info, percpu_pools);
	}
	this_cpu_pools_struct->pcpu_counts[pool_type].in_use--;
	put_cpu();
}

static void nvmeibc_percpu_dma_pool_free_work_fn(struct work_struct *work)
{
	struct nvmeib_dma_percpu_pools *dma_cpu_pools = container_of(work, struct nvmeib_dma_percpu_pools, dma_pool_free_reqs_work);
    BUG_ON(dma_cpu_pools->cpu != smp_processor_id());
	__free_llist_nodes(dma_cpu_pools, /*bound*/ false);
}

static int alloc_local_io_dma_pool(struct nvmeibc_disk *disk, const char *pool_name, struct dma_pool **pool,
    unsigned n_entries, size_t entry_sz, size_t alignment, size_t boundary)
{
    struct device *dma_dev = disk->local_server->dma_device(&disk->local);
    void **ent_virt_arr = NULL;
    dma_addr_t *ent_dma_arr = NULL;
    int i;
    int rv = 0;

    entry_sz = max(entry_sz, sizeof(struct nvmeib_nvme_req_dma_pool_info));

    if (!(*pool = dma_pool_create(
                                pool_name,
                                dma_dev,
                                entry_sz,
                                alignment,
                                boundary)))
    {
        _NE(error_disk_discover_local_prpl_oom,
        "DISK @DISK_NAME (@DISK) could not allocate dma_pool with entry size @SIZE_T and alignment @SIZE_T",
        disk->name, disk, entry_sz, alignment);
        rv = -ENOMEM;
        DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_MEM_ALLOCATION_FAILURE);
        goto out;
    }

    /* Pre-allocate entries for the pool */
    if (!(ent_virt_arr = kcalloc(n_entries, sizeof(*ent_virt_arr), GFP_KERNEL)) ||
            !(ent_dma_arr = kcalloc(n_entries, sizeof(*ent_dma_arr), GFP_KERNEL)))
    {
        _NE(error_2_disk_discover_local_prpl_oom,
        "DISK @DISK_NAME (@DISK) OOM",
        disk->name, disk);
        rv = -ENOMEM;
        DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_MEM_ALLOCATION_FAILURE);
        goto end_pool_alloc;
    }
    /* Because the pool starts empty, we call alloc n_entries times to populate it
    * Note: We must keep all entries outstanding or it will only allocate 1 entry
    */
    for (i = 0; i < n_entries; i++) {
        if (!(ent_virt_arr[i] = dma_pool_alloc(*pool, GFP_KERNEL, &ent_dma_arr[i]))) {
            _NE(error_3_disk_discover_local_prpl_oom,
            "DISK @DISK_NAME (@DISK) could not allocate entry of size @SIZE_T for dma_pool",
            disk->name, disk, entry_sz);
            rv = -ENOMEM;
            DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_DISCOVER_MEM_ALLOCATION_FAILURE);
            break;
        }
    }

    /* Return the newly allocated entries back to the pool */
    for (i--; i >= 0; i--)
        dma_pool_free(*pool, ent_virt_arr[i], ent_dma_arr[i]);

    _NT(trace_alloc_local_io_dma_pool,
        "DISK @DISK_NAME - Allocated DMA pool @STR with @N_ENTS entries of size @SIZE_T",
        disk->name, pool_name, n_entries, entry_sz);

end_pool_alloc:
    kfree(ent_virt_arr);
    kfree(ent_dma_arr);
out:
    return rv;
}

static struct nvmeib_pool_local_free_llist *alloc_local_free_reqs_llist(void)
{
    int cpu;
    struct nvmeib_pool_local_free_llist *this_cpu_llist;
    struct nvmeib_pool_local_free_llist *llist = kzalloc(sizeof(*llist) * nr_cpu_ids, GFP_KERNEL);
    if (!llist) {
        _NE(trace_alloc_percpu_percpu_local_free_reqs_llist, "OOM allocating per-cpu per-cpu local free reqs llist");
        goto out;
    }
    for_each_possible_cpu(cpu) {
        this_cpu_llist = &llist[cpu];
        __local_free_reqs_llist_init(this_cpu_llist);
    }
out:
    return llist;
}

int alloc_percpu_dma_pools(struct nvmeibc_disk *disk)
{
    int rv;
    int cpu;

    if (!(disk->local.dma_pools = nvmeib_public_alloc_percpu_zeroed_cacheline(struct nvmeib_dma_percpu_pools))) {
        _NT(trace_alloc_local_io_dma_pools, "OOM allocating per-cpu dma pools");
        rv = -ENOMEM;
        goto out;
    }

    for_each_possible_cpu(cpu) {
        struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = per_cpu_ptr(disk->local.dma_pools, cpu);
        this_cpu_pools_struct->cpu = cpu;
        INIT_WORK(&this_cpu_pools_struct->dma_pool_free_reqs_work, nvmeibc_percpu_dma_pool_free_work_fn);
        if (!(this_cpu_pools_struct->local_reqs_free_list = alloc_local_free_reqs_llist())) {
            _NE(error_disk_alloc_local_io_dma_pools_local_llist, "OOM allocating per-cpu dma pools local llist");
            rv = -ENOMEM;
            goto free_dma_pools;
        }
    }

    rv = 0;
    goto out;

free_dma_pools:
    for_each_possible_cpu(cpu) {
        struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = per_cpu_ptr(disk->local.dma_pools, cpu);
        if (this_cpu_pools_struct->local_reqs_free_list) {
            kfree(this_cpu_pools_struct->local_reqs_free_list);
            this_cpu_pools_struct->local_reqs_free_list = NULL;
        }
    }
    nvmeib_public_free_percpu(disk->local.dma_pools);
    disk->local.dma_pools = NULL;

out:
    return rv;
}


/* this assumes pools are not used (this is called only on error in creation of each one of the pools)*/
static void __destroy_percpu_dma_pools(struct nvmeibc_disk *disk)
{
    int cpu, i;
    for_each_possible_cpu(cpu) {
        struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = per_cpu_ptr(disk->local.dma_pools, cpu);
        if (this_cpu_pools_struct->local_reqs_free_list) {
            kfree(this_cpu_pools_struct->local_reqs_free_list);
            this_cpu_pools_struct->local_reqs_free_list = NULL;
        }
        for (i = 0; i < NVMEIB_DMA_POOL_TYPE_MAX; i++) {
            if (this_cpu_pools_struct->pools[i].pool) {
                dma_pool_destroy(this_cpu_pools_struct->pools[i].pool);
                this_cpu_pools_struct->pools[i].pool = NULL;
            }
        }
    }
    nvmeib_public_free_percpu(disk->local.dma_pools);
    disk->local.dma_pools = NULL;
}

int alloc_local_io_prpl_pool(struct nvmeibc_disk *disk)
{
    /* Support 1 partial page at start and end */
    int max_prpl_pgs = (disk->max_request_size_bytes >> PAGE_SHIFT) + 2;
    int rv;
    int cpu;


    disk->local.max_prpl_sz = max_prpl_pgs * sizeof(dma_addr_t);
    /* TBD: Set this based on the disk dma_device */
    disk->local.prpl_eof_marker = ~(dma_addr_t)0;

    for_each_possible_cpu(cpu) {
        char pool_name[32];
        struct nvmeib_dma_percpu_pools *cpu_pool_ptr = per_cpu_ptr(disk->local.dma_pools, cpu);
        snprintf(pool_name, sizeof(pool_name), "Local-IO PRPL-%d", cpu);
        if ((rv = alloc_local_io_dma_pool(disk, pool_name,
                                            &cpu_pool_ptr->pools[NVMEIB_DMA_POOL_TYPE_PRPL].pool,
                                            NVMEIB_MAX_NORDDA_IO_REQ,
                                            disk->local.max_prpl_sz,
                                            8, /* alignment requirement for PRPL */
                                            PAGE_SIZE)) < 0) {
            _NE(error_disk_alloc_local_io_prpl_pool, "alloc_local_io_dma_pool failed (@RV)", rv);
            rv = -ENOMEM;
            goto free_pools;
        }
    }
    rv = 0;
    goto out;

free_pools:
    __destroy_percpu_dma_pools(disk);

out:
    return rv;
}

int alloc_local_io_rd_md_pool(struct nvmeibc_disk *disk)
{
    int rv;
    int cpu;


    for_each_possible_cpu(cpu) {
        struct nvmeib_dma_percpu_pools *cpu_pool_ptr = per_cpu_ptr(disk->local.dma_pools, cpu);
        char pool_name[32];
        snprintf(pool_name, sizeof(pool_name), "Local-IO Read-MD Data-%d", cpu);
        if ((rv = alloc_local_io_dma_pool(disk, pool_name,
                                            &cpu_pool_ptr->pools[NVMEIB_DMA_POOL_TYPE_RD_MD].pool,
                                            NVMEIB_MAX_NORDDA_IO_REQ,
                                            disk->max_request_size_bytes,
                                            PAGE_SIZE, /* alignment requirement for data pages */
                                            0)) < 0) {
            _NE(error_disk_alloc_local_io_rd_md_pool, "alloc_local_io_dma_pool failed (@RV)", rv);
            rv = -ENOMEM;
            goto free_pools;
        }
    }

    rv = 0;
    goto out;

free_pools:
    __destroy_percpu_dma_pools(disk);

out:
    return rv;
}

int alloc_local_io_md_dma_pool(struct nvmeibc_disk *disk)
{
    int rv;
    int cpu;


    disk->local.md_dma_pool_entry_sz = NVMEIBC_D2MD_LEN(disk->max_request_size_bytes, disk);

    for_each_possible_cpu(cpu) {
        char pool_name[32];
        struct nvmeib_dma_percpu_pools *cpu_pool_ptr = per_cpu_ptr(disk->local.dma_pools, cpu);
        snprintf(pool_name, sizeof(pool_name), "Local-IO Dummy MD-%d", cpu);
        if ((rv = alloc_local_io_dma_pool(disk, pool_name,
                                            &cpu_pool_ptr->pools[NVMEIB_DMA_POOL_TYPE_DUMMY_MD].pool,
                                            NVMEIB_MAX_NORDDA_IO_REQ,
                                            disk->local.md_dma_pool_entry_sz,
                                            8, /* alignment requirement for MD */
                                            PAGE_SIZE)) < 0) {
            _NE(error_disk_alloc_local_io_md_dma_pool, "alloc_local_io_dma_pool failed (@RV)", rv);
            rv = -ENOMEM;
            goto free_pools;
        }
    }

    rv = 0;
    goto out;

free_pools:
    __destroy_percpu_dma_pools(disk);

out:	
    return rv;
}

static void __empty_target_local_llist(int target_cpu, struct nvmeib_pool_local_free_llist *local_reqs_free_list_array, struct nvmeib_dma_percpu_pools __percpu *percpu_pools_struct, int came_from_cpu)
{
	struct llist_node *node = NULL;
	struct nvmeib_pool_local_free_llist *target_cpu_local_llist = NULL;
	struct nvmeib_dma_percpu_pools *target_cpu_pools_struct = per_cpu_ptr(percpu_pools_struct, target_cpu);
	if (!local_reqs_free_list_array) {
		/* was never initialized */
		goto out;
	}

	target_cpu_local_llist = &local_reqs_free_list_array[target_cpu];
	if (target_cpu_local_llist) {
		node = llist_del_all(&target_cpu_local_llist->head);
		if (node) {
			nvmeib_public_llist_add_batch(node, target_cpu_local_llist->last, &target_cpu_pools_struct->reqs_free_list);
		}
	}

out:
	return;
}

void free_percpu_pools(struct nvmeibc_disk *disk, struct nvmeib_dma_percpu_pools __percpu *percpu_pools)
{
	int cpu, target_cpu;
	enum nvmeib_dma_pool_type pool_type;
	struct nvmeib_dma_percpu_pools *this_cpu_pools_struct = NULL;

	if (!percpu_pools) {
		/* was never initialized */
		goto out;
	}
	BUG_ON(!disk->io_stopped);

	// as we are disconnecting, it is assumed here that the reqs_free_list cant grow at this point, only shrink. Cancel all outstanding works and free everything here.
	for_each_possible_cpu(cpu) {
		this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu);
		nvmeib_public_cancel_work_sync(&this_cpu_pools_struct->dma_pool_free_reqs_work);
	}

	// now that there's no inflight works, we can empty all percpu percpu local free lists into their target cpu free list without the fear from race
	for_each_possible_cpu(cpu) {
		this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu);
		for_each_possible_cpu(target_cpu) {
			__empty_target_local_llist(target_cpu, this_cpu_pools_struct->local_reqs_free_list, percpu_pools, cpu);
		}
	}

	// then finally call the free_work_fn on all cpus one last time
	for_each_possible_cpu(cpu) {
		this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu);
		__free_llist_nodes(this_cpu_pools_struct, /*bound*/ false);
		for (pool_type = 0; pool_type < NVMEIB_DMA_POOL_TYPE_MAX; pool_type++) {
			if (this_cpu_pools_struct->pools[pool_type].pool) {
				dma_pool_destroy(this_cpu_pools_struct->pools[pool_type].pool);
				this_cpu_pools_struct->pools[pool_type].pool = NULL;
			}
		}
		if (this_cpu_pools_struct->local_reqs_free_list) {
			kfree(this_cpu_pools_struct->local_reqs_free_list);
			this_cpu_pools_struct->local_reqs_free_list = NULL;
		}
	}
	
	// sanity check for 6264
	for (pool_type = 0; pool_type < NVMEIB_DMA_POOL_TYPE_MAX; pool_type++) {
		int cpu, tot_use_cnt = 0;
		struct nvmeib_pool_percpu_counts *pcpu_counts;
		struct nvmeib_dma_percpu_pools *this_cpu_pools_struct;
		for_each_possible_cpu(cpu) {
			this_cpu_pools_struct = per_cpu_ptr(percpu_pools, cpu);
			if (this_cpu_pools_struct->pools[pool_type].pool) {
				pcpu_counts = &this_cpu_pools_struct->pcpu_counts[pool_type];
				tot_use_cnt += pcpu_counts->in_use;
			}
		}
	
		if (tot_use_cnt != 0) {
			_NW_dmesg(warn_disk_disconnect_disk_prpl_count_not_zero, 
					"Local Disk @DISK_NAME(@DISK) has non-zero in_use count "
					"for pool type @INT", disk->name, disk, pool_type);
			BUG_NON_PRODUCTION(6264);
		}
	}

	nvmeib_public_free_percpu(percpu_pools);

out:
	return;
}
