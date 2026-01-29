/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * manages disk locks fuctionality
 *
 * 5/2015
 */

#include <linux/list.h>
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_defs.h"
#include "nvmeib_utils.h"
#include "nvmeibs_client.h"
#include "nvmeib_types.h"
#include "nvmeibs_serjio.h"
#include "nvmeibs_trace.h"
#include "nvmeibs_nvme.h"
#include "nvmeib_public.h"
#include "nvmeib_numa.h"
#include <linux/mm.h>
#include "nvmeibs_memmgr_metrics.h"

NVMEIBS_MEMMGR_METRIC(disks_locks, "component=target.disks.locks");

static DEFINE_MUTEX(locks_guard);

static int disk_lock_allocate_(struct nvmeibs_disk_info *di, int seg_id,
	u64 start, u64 lock_set_size, u64 len, u64 init_val);
static void nvmeibs_disk_lock_dev_set_(struct nvmeibs_disk_info *di,
	struct nvmeibs_dev *lock_dev, bool is_toma);
static void nvmeibs_disk_lock_dev_get_(struct nvmeibs_disk_info *di,
	bool is_toma);

static u32 map_on_dev(struct nvmeibs_dev *nic,
	struct nvmeib_alloc_n_map *mem)
{
	u32 ret_rkey = 0;

	NFIN;

	mem->pd = nic->dev->pd;
	mem->access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;
	mem->ioaddr = 0; /*we are using the entire memory*/
	mem->dma_dir = DMA_BIDIRECTIONAL;
	_NT(trace_0_map_on_dev, "LOCKS: map (@PTR) @INT pages on dev=@DEVICE_NAME",
		mem, mem->n_pages, mem->pd->device->name);

	if (nvmeib_mem_alloc_n_map(mem) == 0) {
		ret_rkey = mem->rkey;
		_NT(trace_disk_locks_map_on_dev, "ret_rkey=@RET_RKEY, pd=@PD, access=@ACCESS, device_name=@DEVICE_NAME, pages=@PAGES, sgl=@PTR",
			mem->rkey, mem->pd, mem->access_flags,
			mem->pd->device->name, mem->pages, mem->mem_table.sgl);
	}
	else
		_NE(error_disk_locks_map_on_dev, "memory map allocation failed");

	NFOUT;
	return ret_rkey;
}

/*
log the device attributres that are linked to atomic operations
*/
static void log_device_atomic_params(struct ib_device *dev)
{
	struct ib_device_attr attrs;
	NFIN;

	if (nvmeib_query_device(dev, &attrs)) {
		_NE(error_disk_locks_log_device_atomic_params, "LOCKS: cannot log the device locks attributes");
		goto out;
	}

	_NT(trace_disk_locks_log_device_atomic_params, "LOCKS: dev attributes of device @DEV_NAME", dev->name);
	_NT(trace_1_disk_locks_log_device_atomic_params, "LOCKS: Atomic capatbility @ATOMIC_CAP", attrs.atomic_cap);
	_NT(trace_2_disk_locks_log_device_atomic_params, "LOCKS: Max_qp_rd_atom @MAX_QP_RD_ATOM", attrs.max_qp_rd_atom);
	_NT(trace_3_disk_locks_log_device_atomic_params, "LOCKS: max_ee_rd_atom @MAX_EE_RD_ATOM", attrs.max_ee_rd_atom);
	_NT(trace_4_disk_locks_log_device_atomic_params, "LOCKS: max_res_rd_atom @MAX_RES_RD_ATOM", attrs.max_res_rd_atom);
	_NT(trace_5_disk_locks_log_device_atomic_params, "LOCKS: max_ee_init_rd_atpm @MAX_EE_INIT_RD_ATOM", attrs.max_ee_init_rd_atom);
	_NT(trace_6_disk_locks_log_device_atomic_params, "LOCKS: masked_atomic_cap @MASKED_ATOMIC_CAP", attrs.masked_atomic_cap);

out:
	NFOUT;
	return;
}


/**
 * initialize lock which is maintained per device in a given
 * nvmeibs_disk_lock_mem_info
 *
 * @param lmi - the managed lock
 *
 * @return int
 */
/* Called with disks lock taken */
static int disk_map_mem_lock_at_device(struct nvmeibs_dev *nic,
	struct nvmeibs_disk_lock_mem_info *lmi)
{
	int ret = 0;
	struct nvmeibs_disk_lock_mem_info_for_device *this_lock_dev = NULL;

	NFIN;
	/* Take disk_locks lock - (Not to be confused with disks lock already taken) */
	nvmeibs_disk_locks_guard();
	log_device_atomic_params(nic->dev->ib_dev);
	this_lock_dev = kzalloc(sizeof(*this_lock_dev), GFP_KERNEL);
	if (!this_lock_dev) {
		_NE(error_disk_locks_disk_map_mem_lock_at_device, "Memory allocation problem");
		ret = -EINVAL;
		goto out_err;
	}
	this_lock_dev->mem.pages = lmi->pages;
	this_lock_dev->mem.n_pages = lmi->size;
	this_lock_dev->rkey = map_on_dev(nic, &this_lock_dev->mem);
	if (this_lock_dev->rkey == 0) {
		ret = -ENOMEM;
		_NE(error_1_disk_locks_disk_map_mem_lock_at_device, "Failed to allocate memory on device");
		goto out_err;
	}
	nvmeib_mem_sync_map_for_device(&this_lock_dev->mem, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	this_lock_dev->lkey = this_lock_dev->mem.mr->lkey;
	this_lock_dev->nic_dev = nic;
	this_lock_dev->lmi = lmi;
	++lmi->num_of_device_locks;
	list_add_tail(&this_lock_dev->entry, &lmi->per_device_locks);
	goto out;

out_err:
	_ND(error_2_disk_locks_disk_map_mem_lock_at_device, "LOCKS: nvmeibs_disk_map_mem_lock_at_devices error!!!!");
	if (this_lock_dev && this_lock_dev->rkey)
		nvmeib_mem_unmapn_n_free(&this_lock_dev->mem);
	if (this_lock_dev) {
		kfree(this_lock_dev);
		this_lock_dev = NULL;
	}

out:
	nvmeibs_disk_lock_unguard();
	NFOUT;
	return ret;
}

/* Called with disks lock taken */
int nvmeibs_disk_lock_disk_map_segs_for_dev_(
	struct nvmeibs_disk_info *di, struct nvmeibs_dev *nic)
{
	struct nvmeibs_disk_private_data *disk_pd;
	struct nvmeibs_disk_lock_mem_info *lmi;
	int rv = 0;

	NFIN;
	_ND(trace_disk_locks_nvmeibs_disk_lock_disk_map_segs_for_dev, "LOCKS: mapping segments for disk @DISK_ID_STR", di->disk_id);
	disk_pd = di->priv;
	if (!disk_pd) {
		_NE(error_disk_locks_nvmeibs_disk_lock_disk_map_segs_for_dev, "LOCKS: FATAL: empty disk info of disk @DISK_ID_STR", di->disk_id);
		rv = -ENODEV;
		goto out;
	}
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		_ND(trace_1_disk_locks_nvmeibs_disk_lock_disk_map_segs_for_dev, "LOCKS: mapping memory for disk @DISK_ID_STR on segment @SEG_ID_INT",
			di->disk_id, lmi->seg_id);
		if ((rv = disk_map_mem_lock_at_device(nic, lmi)))
			goto out;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_disk_lock_disks_map_segs_for_dev(struct nvmeibs_dev *nic)
{
	int rv = 0, n_disks;
	struct list_head *disks;
	struct nvmeibs_disk_info *di;

	NFIN;
	disks = nvmeibs_disk_get_disks(&n_disks);
	_ND(trace_disk_locks_nvmeibs_disk_lock_disks_map_segs_for_dev, "LOCKS: found @N_DISKS disks", n_disks);
	list_for_each_entry(di, disks, link)
		if ((rv = nvmeibs_disk_lock_disk_map_segs_for_dev_(di, nic)))
			goto out;
	_ND(trace_1_disk_locks_nvmeibs_disk_lock_disks_map_segs_for_dev, "LOCKS: finished to map lock memory at device @DEV_NAME",
		nic->dev->ib_dev->name);

out:
	nvmeibs_disk_put_disks();
	NFOUT;
	return rv;
}



int nvmeibs_disk_alloc_lock_memory(struct nvmeibs_disk_info *di,
	int seg_id, u64 start, u64 lock_set_size, u64 len, u64 init_val)
{
	int rv = 0;
	struct nvmeibs_disk_private_data *disk_pd __attribute__((unused));

	rv = disk_lock_allocate_(di, seg_id, start, lock_set_size, len, init_val);
	if (rv)
		goto out;

	disk_pd = di->priv;
	goto out;

out:
	return rv;
}

static inline size_t calc_lmi_alloc_sz(struct nvmeibs_disk_lock_mem_info *lmi)
{
	return sizeof(*lmi) + (lmi->size * sizeof(*lmi->pages)) + (lmi->size * PAGE_SIZE);
}

/**
 * initialize disk lock memory the information about the lock is
 * stored in disk resources units are given in blocks, that is
 * in 4096 bytes
 *
 * @param disk the disk to be used
 * @param start
 * @param len - number of blocks in disk segment
 * @param lock_set_size
 */
static int disk_lock_allocate_(struct nvmeibs_disk_info *di, int seg_id,
	u64 start, u64 lock_set_size, u64 len, u64 init_val)
{
	struct nvmeibs_disk_private_data *disk_pd;
	struct nvmeibs_disk_lock_mem_info *lmi = NULL;
	u64 size, num_of_locks;
	int ret = 0;
	struct nvmeib_numa_iter niter;
	int cpu_packages = 1;

	NFIN;
	/*locate disk_*/

	disk_pd = di->priv;
	if (!disk_pd) {
		_NE(error_disk_locks_disk_lock_allocate, "LOCKS: FATAL: empty disk info of disk @DISK_ID_STR", di->disk_id);
		ret = -ENODEV;
		goto err_out;
	}
	if (disk_pd->n_memsegs == 0)
		INIT_LIST_HEAD(&disk_pd->lock_mems);
	lmi = kzalloc(sizeof(*lmi), GFP_KERNEL);
	if (!lmi) {
		_NE(error_1_disk_locks_disk_lock_allocate, "Cannot allocate disk memory for lock segment");
		ret = -ENOMEM;
		goto err_out;
	}

	lmi->start_addr = start;
	lmi->seg_id = seg_id;
	lmi->lock_set_size = lock_set_size;
	lmi->len = len;
	num_of_locks = DIV_ROUND_UP(len,  lock_set_size);	// number of locks entries
	size = DIV_ROUND_UP(num_of_locks * NVMEIB_LOCK_BLKSET_ENTRY_SIZE, PAGE_SIZE);	// size in units of pages
	_NT(trace_disk_locks_disk_lock_allocate, "LOCKS: disk @DISK_ID_STR, alloc lock-tbl of @SIZE_LLONG pages", di->disk_id, size);
	lmi->size = size;
	if (!(lmi->pages = kvmalloc(size * sizeof(*lmi->pages), GFP_KERNEL))) {
		_NE(error_2_disk_locks_disk_lock_allocate, "Pages memory allocation error for disk @DISK_ID_STR", di->disk_id);
		ret = -ENOMEM;
		goto err_free;
	}
	nvmeib_numa_iter_init(&niter, nvmeib_numa_alloc_policy,
	                      nvmeibs_disk_info_get_nvme_dma_device(di));

#if defined(__i386__) || defined(__x86_64__)
	cpu_packages = topology_max_packages();
#endif

	_NT(trace_3_disk_locks_disk_lock_allocate,
		"LOCKS: lock-table alloc policy used @INT (cfg=@INT), num-online-nodes=@INT, num-packages=@INT",
		niter.policy, nvmeib_numa_alloc_policy, num_node_state(N_ONLINE), cpu_packages);
	ret = nvmeib_numa_iter_alloc_n_pages(
	    &niter, lmi->pages, lmi->size, init_val,
	    GFP_KERNEL | __GFP_ZERO);
	if (ret) {
		ret = -ENOMEM;
		goto err_free;
	}
	_NT(trace_2_disk_locks_disk_lock_allocate,
			"LOCKS: disk @DISK_NAME LMI @LMI pages @PAGES - allocated @SIZE_LLONG pages",
			di->disk_id, lmi, lmi->pages, lmi->size);
	++disk_pd->n_memsegs;
	list_add_tail(&lmi->link, &disk_pd->lock_mems);
	INIT_LIST_HEAD(&lmi->per_device_locks);
	lmi->num_of_device_locks = 0;

	nvmesh_memmgr_metric_on_alloc_update(disks_locks, calc_lmi_alloc_sz(lmi), true /* success */);
	goto out;

err_free:
	kvfree(lmi->pages);
	kfree(lmi);
err_out:
	_NE(error_4_disk_locks_disk_lock_allocate, "LOCKS: error when trying to allocate locks for disk name @DISK_ID_STR",
		di->disk_id);

	nvmesh_memmgr_metric_on_alloc_update(disks_locks, 0, false /* success */);
out:
	NFOUT;
	return ret;
}

static void free_lock_on_seg(struct nvmeibs_disk_lock_mem_info *lmi,
	struct nvmeibs_dev *nic);

void nvmeibs_disk_lock_unmap_on_dev(struct nvmeibs_dev *nic)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	struct nvmeibs_disk_private_data *disk_pd = 0;
	struct nvmeibs_disk_info *di;
	struct list_head *disks = nvmeibs_disk_get_disks(NULL);

	NFIN;
	list_for_each_entry(di, disks, link) {
		disk_pd = di->priv;
		if (disk_pd) {
			list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
				free_lock_on_seg(lmi, nic);
			}
		}
	}


	nvmeibs_disk_put_disks();
	NFOUT;
}

/**
 *
 * free all memory maps of a disks on all local nics
 *
 * @param lmi
 * @param nic - NIC to unmap from (if NULL, unmap from all NICs)
 */
static void free_lock_on_seg(struct nvmeibs_disk_lock_mem_info *lmi,
	struct nvmeibs_dev *nic)
{
	struct nvmeibs_disk_lock_mem_info_for_device *this_lock_dev, *tmp;

	NFIN;
	nvmeibs_disk_locks_guard();
	_ND(trace_disk_locks_free_lock_on_seg, "LOCKS: free locks on device with ptr @LMI", lmi);
	list_for_each_entry_safe(this_lock_dev, tmp, &lmi->per_device_locks, entry){
		if (!this_lock_dev) {
			_NE(error_disk_locks_free_lock_on_seg, "LOCKS: FATAL null device in devices list");
			continue;
		}
		if (nic && this_lock_dev->nic_dev != nic) {
			continue;
		}

		nvmeib_mem_unmapn_n_free(&this_lock_dev->mem);
		list_del(&this_lock_dev->entry);
		_ND(trace_1_disk_locks_free_lock_on_seg, "LOCKS: free lock_dev @THIS_LOCK_DEV", this_lock_dev);
		kfree(this_lock_dev);
	}
	nvmeibs_disk_lock_unguard();
	NFOUT;
}

void nvmeibs_disk_free_lock_memory(struct nvmeibs_disk_private_data *disk_pd)
{
	/*assuming locked guard*/
	struct nvmeibs_disk_lock_mem_info *lmi;

	NFIN;
	if (!disk_pd) {
		_NI(trace_disk_locks_nvmeibs_disk_free_lock_memory, "LOCKS: Oops disk_pd NULL");
		goto out;
	}
	_NT(trace_1_disk_locks_nvmeibs_disk_free_lock_memory, "LOCKS: disk_pd @DISK_PD, n_memsegs = @N_MEMSEGS", disk_pd, disk_pd->n_memsegs);
	while ((lmi = list_first_entry_or_null(&disk_pd->lock_mems,
		struct nvmeibs_disk_lock_mem_info, link)) != NULL) {
		_NT(trace_2_disk_locks_nvmeibs_disk_free_lock_memory,
				"LOCKS: disk_pd @DISK_PD, lmi @LMI, pages @PAGES",
				disk_pd, lmi, lmi->pages);
		free_lock_on_seg(lmi, NULL);

		if (lmi->pages) {
			nvmeib_numa_iter_free_n_pages(lmi->pages, lmi->size);
			kvfree(lmi->pages);
			lmi->pages = NULL;
		}

		list_del(&lmi->link);
		nvmesh_memmgr_metric_on_free_update(disks_locks, calc_lmi_alloc_sz(lmi));
		kfree(lmi);
	}

	disk_pd->n_memsegs = 0;

out:
	NFOUT;
	return;
}

void nvmeibs_update_lock_segments(struct nvmeibs_disk_private_data *disk_pd)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	struct nvmeibs_disk_lock_mem_info_for_device *this_mem_info_for_dev;
	bool found = false;
	NFIN;

	// keep this assert for future references
	BUG_ON(nvmeibs_use_tcp_locks);

	BUG_ON(!mutex_is_locked(&locks_guard));
	_ND(trace_disk_locks_nvmeibs_update_lock_segments, "Appr");
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		found = false;
		list_for_each_entry(this_mem_info_for_dev, &lmi->per_device_locks,
			entry) {
			if (this_mem_info_for_dev->nic_dev == disk_pd->lock_dev)  {
				lmi->selected = this_mem_info_for_dev;
				found = true;
				break;
			}
		}
		BUG_ON(!found);
	}

	NFOUT;
}

static void ldisk_ldl_list_free(struct nvmeib_local_disk *ldisk)
{
	NFIN;
	if (ldisk->ldl_a) {
		INIT_LIST_HEAD(&ldisk->ldl_list);
		ldisk->n_ldl = 0;
		kfree(ldisk->ldl_a);
		ldisk->ldl_a = NULL;
	}
	NFOUT;
}

static void init_ldl(struct nvmeib_local_disk_locks *ldl,
	struct nvmeibs_disk_lock_mem_info *lmi,
	struct nvmeibs_disk_lock_mem_info_for_device *main_dev,
	u64 lock_id)
{
	NFIN;

	ldl->len = lmi->len;
	ldl->rkey = main_dev->rkey;
	ldl->lock_set_size = lmi->lock_set_size;
	ldl->seg_id = lmi->seg_id;
	ldl->start_addr = lmi->start_addr;
	ldl->addr = main_dev->mem.ioaddr;
	ldl->lock_id = lock_id;
	ldl->pages = lmi->pages;

	NFOUT;
}

struct nvmeibs_disk_lock_mem_info_for_device *
nvmeibs_disk_locks_lmi_get_device(
	struct nvmeibs_disk_lock_mem_info *lmi, u8 raw[16],
	enum rdma_link_layer layer,
	enum rdma_transport_type transport)
{
	struct nvmeibs_disk_lock_mem_info_for_device *dev = NULL;
	struct nvmeibs_ib_port *port;
	bool found = false;
	char gid_str[GUID_SIZE] __attribute__ ((unused)) = {0};

	NFIN;
	format_gid_raw(raw, gid_str);
	_ND(trace_disk_locks_nvmeibs_disk_locks_lmi_get_device, "Looking for GID @GID_STR", gid_str);
	nvmeibs_get_devices(NULL);
	nvmeibs_disk_locks_guard();
	list_for_each_entry(dev, &lmi->per_device_locks, entry) {
		list_for_each_entry(port, &dev->nic_dev->port_list, port_list_n) {
			if (!memcmp(port->gid.gid.raw, raw, 16)) {
				if (port->layer == layer && port->transport == transport) {
					_NT(trace_1_disk_locks_nvmeibs_disk_locks_lmi_get_device,
						"dev=@DEV rkey=@RKEY name=@IB_NAME"
						"gid=@GID_STR layer=@INT transport=@INT",
						dev, dev->rkey, dev->nic_dev->dev->ib_dev->name,
						gid_str, layer, transport);
					found = true;
					goto out;
				}
				else {
					/* JH: This won't work.
					   Let's say we have a NIC that supports RoCE and iWARP in
					   Hardware or alternatively MLX atomics with PCIe, then
					   this would be fine. It's only because SIW does everything
					   in SW and MLX does the atomics on NIC that it is a problem. */
					_NT(trace_2_disk_locks_nvmeibs_disk_locks_lmi_get_device,
						"Skip matching gid, layer and/or transport differ:"
						"name=@IB_NAME, gid=@GID_STR {layer, transport}: "
						"{@INT,@INT} vs, {@INT,@INT}",
						dev->nic_dev->dev->ib_dev->name, gid_str,
						port->layer, port->transport, layer, transport);
				}
			}
		}
	}
	if (!found)
		dev = NULL;

out:
	nvmeibs_disk_lock_unguard();
	nvmeibs_put_devices();
	NFOUT;
	return dev;
}

static int ldisk_ldl_list_alloc(struct nvmeibs_client *cl,
	struct nvmeib_local_disk *ldisk, struct nvmeibs_disk_private_data *disk_pd)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	u64 lock_id = atomic64_inc_return(&disk_pd->disk_lock_counter) + 1;
	int i;
	int rv = -1;
	struct nvmeibs_disk_lock_mem_info_for_device *selected_dev = NULL;
	struct nvmeibs_disk_lock_mem_info_for_device *main_dev = NULL;

	NFIN;
	if (ldisk->ldl_a) {
		_NE(error_disk_locks_ldisk_ldl_list_alloc, "Disk prev ldisk-locks' ldl not freed (@LDL_A)", ldisk->ldl_a);
		goto out;
	}
	if (!disk_pd->n_memsegs) {
		_NE(error_1_disk_locks_ldisk_ldl_list_alloc, "Disk has 0 lock-segs");
		goto out;
	}

	INIT_LIST_HEAD(&ldisk->ldl_list);
	ldisk->n_ldl = disk_pd->n_memsegs;
	if (!(ldisk->ldl_a =
		  kzalloc(sizeof(ldisk->ldl_a[0]) * ldisk->n_ldl, GFP_KERNEL))) {
		_NE(error_2_disk_locks_ldisk_ldl_list_alloc, "Fail to allocated ldl arr");
		goto out;
	}
	i = 0;
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		if (i == disk_pd->n_memsegs) {
			_NE(error_3_disk_locks_ldisk_ldl_list_alloc, "lmi list > cnt (@N_MEMSEGS)", disk_pd->n_memsegs);
			goto err;
		}
		selected_dev = lmi->selected;
		main_dev = nvmeibs_disk_locks_lmi_get_device(
			lmi, cl->lock_net->params.port->gid.gid.raw,
			cl->lock_net->params.port->layer, cl->lock_net->params.port->transport);
		if (selected_dev == main_dev)
			_ND(trace_disk_locks_ldisk_ldl_list_alloc, "Lock device is the main locking device");
		else
			_ND(trace_1_disk_locks_ldisk_ldl_list_alloc, "Lock device is NOT the main locking device");
		if (main_dev) {
			init_ldl(&ldisk->ldl_a[i], lmi, main_dev, lock_id);
			list_add_tail(&ldisk->ldl_a[i].link, &ldisk->ldl_list);
			i++;
		}
		else {
			//YK: why cant we have lmi->selected==false?
			_NE(error_4_disk_locks_ldisk_ldl_list_alloc, "seg @SEG_ID_INT has no lock-dev, prob all devs are down", lmi->seg_id);
			goto err;
		}
	}
	if (i < disk_pd->n_memsegs) {
		_NE(error_5_disk_locks_ldisk_ldl_list_alloc, "lmi list < cnt (@N_MEMSEGS)", disk_pd->n_memsegs);
		goto err;
	}

	rv = 0;
	goto out;

err:
	ldisk_ldl_list_free(ldisk);

out:
	NFOUT;
	return rv;
}

int nvmeibs_disk_locks_ldisk_alloc(struct nvmeibs_client *cl,
	struct nvmeib_local_disk *ldisk)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *disk_pd;
	int rv = -1;
	NFIN;

	/* Not locking the disks or devs as we hold refcnt for both di and lock-dev.
	   This means that both wait for us before remove. plus we only read info */

	if (!(di = cl->di) || !(disk_pd = di->priv)) {
		_NE(error_disk_locks_nvmeibs_disk_locks_ldisk_alloc, "Invalid input (di=@DI)", cl->di);
		goto out;
	}

	if (ldisk_ldl_list_alloc(cl, ldisk, disk_pd) < 0) {
		_NE(error_1_disk_locks_nvmeibs_disk_locks_ldisk_alloc, "Fail to allocated ldisk ldl list");
		goto out;
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

int nvmeibs_disk_mmap_fault(void *arg, unsigned long pg_offset,
							struct page **page)
{
	struct nvmeibs_disk_private_data *disk_pd = arg;
	struct nvmeibs_disk_lock_mem_info *lmi;
	bool found = false;
	int rv = -1;
	//NFIN;

	nvmeibs_disk_locks_guard();

	//find lmi of the disk segment
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		if (lmi->seg_id == 0) { //server has only single segment, #0
			found = true;
			break;
		}
	}
	if (!found) {
		_NE(error_disk_locks_nvmeibs_disk_mmap_fault, "lmi not found");
		goto unlock_g;
	}

	//check requested region is within lock table pages
	if (pg_offset >= lmi->size) {
		_NE(error_1_disk_locks_nvmeibs_disk_mmap_fault, "mmap requested pg offset @PG_OFFSET spans out of disk "
		   "lock table #pages (@SIZE_LLONG)", pg_offset, lmi->size);
		goto unlock_g;
	}

	*page = lmi->pages[pg_offset];
	rv = 0;

unlock_g:
	nvmeibs_disk_lock_unguard();

	//NFOUT;
	return rv;
}

void nvmeibs_disk_locks_guard(void)
{
	NFIN;

	mutex_lock(&locks_guard);
	NFOUT;
}

void nvmeibs_disk_lock_unguard(void)
{
	NFIN;

	mutex_unlock(&locks_guard);
	NFOUT;
}

bool nvmeibs_disk_locks_is_selected_device(struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port *ib_port, struct nvmeibs_client *cl)
{
	struct nvmeibs_disk_private_data *disk_private_data = di->priv;
	bool rv = false;

	NFIN;

	if (!di) {
		_NE(error_disk_locks_nvmeibs_disk_locks_is_selected_device, "Oops: di is NULL");
		goto out;
	}
	nvmeibs_disk_locks_guard();
	if (!cl->lock_validated) {
		if (likely(!nvmeibs_use_tcp_locks)) {
			if (disk_private_data->lock_dev == NULL) {
				nvmeibs_disk_lock_dev_set_(di, ib_port->nis_dev, false);
				rv = true;
			}
			else if (nvmeib_same_physical_dev(ib_port->nis_dev->dev,
				disk_private_data->lock_dev->dev)) {
				nvmeibs_disk_lock_dev_get_(di, false);
				rv = true;
			}
			else
				rv = false;
		} else {
			BUG_ON(disk_private_data->lock_dev);
			rv = true;
		}

		if (rv)
			cl->lock_validated = true;
	} else
		_NT(trace_disk_locks_nvmeibs_disk_locks_is_selected_device, "Rejected because cl's lock-ch already exists");
	nvmeibs_disk_lock_unguard();

out:
	NFOUT;
	return rv;
}

/* Called with the nvmeibs_dev_guard lock taken */
static struct nvmeibs_ib_port *find_active_port(struct nvmeibs_dev *lock_dev)
{
	struct nvmeibs_ib_port *port = NULL;
	struct ib_port_attr a;
	bool found = false;

	NFIN;
	list_for_each_entry(port, &lock_dev->port_list, port_list_n) {
		 if (ib_query_port(P2IB(port), port->port, &a)) {
			 _NE(error_disk_locks_find_active_port, "ib_query_port() failed.");
			 continue;
		 }
		 if (a.state == IB_PORT_ACTIVE) {
			 /*do not want use roce port without ip addr*/
			 if (!port->gid.valid) {
				_NW(warn_disk_locks_find_active_port, "Not using port with invalid GID. (port @HW_GID)", &port->gid.hw_gid);
				continue;
			 }
			 found = true;
			 break;
		 }
	}
	if (!found) {
		port = NULL;
	}

	NFOUT;
	return port;
}

/* check no disk is using this @dev as its lock-dev */
bool nvmeibs_disk_locks_lock_dev_is_used(struct nvmeibs_dev *dev)
{
	struct nvmeibs_disk_info *di;
	struct list_head *disk_info_list;
	struct nvmeibs_disk_private_data *disk_pd;
	bool rv = false;
	NFIN;

	disk_info_list = nvmeibs_disk_get_disks(NULL);
	nvmeibs_disk_locks_guard();
	list_for_each_entry(di, disk_info_list, link) {
		disk_pd = di->priv;
		if (dev == disk_pd->lock_dev) {
			_NE(error_disk_locks_nvmeibs_disk_locks_lock_dev_is_used, "Disk @DISK_ID_STR still uses lock dev @IB_DEV_NAME (@DEV), refcnt @LOCK_DEV_REFCNT(@LOCK_DEV_REFCNT_TOMA)",
				di->disk_id, N2IB(dev)->name, dev,
				disk_pd->lock_dev_refcnt, disk_pd->lock_dev_refcnt_toma);
			rv = true;
		}
	}
	nvmeibs_disk_lock_unguard();
	nvmeibs_disk_put_disks();

	NFOUT;
	return rv;
}

/**
 * Get (or set) the lock-device - Used by either Toma or Local-client.
 * Toma inc ref-cnt whereas Local-client will do the same while connecting
 * loopback lock-channel (on port-wq).
 */
int nvmeibs_disk_lock_get_local_dev(
	struct nvmeibs_disk_info *di, struct nvmeibs_ib_port **l_port,
	int n_ports, bool is_toma)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	struct nvmeibs_dev *lock_dev;
	struct nvmeibs_dev *dev;
	int num_devs;
	struct list_head *devs;
	int num_ports = 0;

	NFIN;
	BUG_ON(disk_pd == NULL);
	if (n_ports < 1) {
		_NE(error_disk_locks_nvmeibs_disk_lock_get_local_dev, "Port array size @N_PORTS is too small", n_ports);
		goto outt;
	}
	devs = nvmeibs_get_devices(&num_devs);
	nvmeibs_disk_locks_guard();
	lock_dev = disk_pd->lock_dev;
	if (nvmeibs_use_tcp_locks) {
		BUG_ON(lock_dev);
	} else if (!lock_dev) {
		*l_port = NULL;
		list_for_each_entry(lock_dev, devs, nvmeibs_dev_list_n) {
			*l_port = find_active_port(lock_dev);
			if (*l_port)
				break;
		}
		if (!(*l_port)) {
			_NE(error_1_disk_locks_nvmeibs_disk_lock_get_local_dev, "No active port for lock-dev");
			lock_dev = NULL;
			*l_port = NULL;
			goto out;
		}
		num_ports = 1;
		nvmeibs_disk_lock_dev_set_(di, lock_dev, is_toma);
	} else {
		list_for_each_entry(dev, devs, nvmeibs_dev_list_n) {
			if (nvmeib_same_physical_dev(dev->dev, lock_dev->dev)) {
				if (num_ports < n_ports) {
					l_port[num_ports] = find_active_port(dev);
					if (l_port[num_ports])
						num_ports++;
				}
				else {
					_NE(error_2_disk_locks_nvmeibs_disk_lock_get_local_dev, "Port array @N_PORTS is too small", n_ports);
					num_ports = 0;
					break;
				}
			}
		}
		if (num_ports)
			nvmeibs_disk_lock_dev_get_(di, is_toma);
		else
			_NE(error_3_disk_locks_nvmeibs_disk_lock_get_local_dev, "No active ports on curr lock dev @IB_DEV_NAME (@LOCK_DEV), refcnt=@REFCNT",
				N2IB(disk_pd->lock_dev)->name, lock_dev,
				disk_pd->lock_dev_refcnt);
	}

out:
	nvmeibs_disk_lock_unguard();
	nvmeibs_put_devices();

outt:
	NFOUT;
	return num_ports;
}

int nvmeibs_disk_locks_get_dev(char *selected_disk_name, union ib_gid *out_gids,
	int *num_gids)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;
	int n_lock_ports = *num_gids < MAX_PORTS_FOR_LOCKS_GIDS ?
		*num_gids : MAX_PORTS_FOR_LOCKS_GIDS;
	struct nvmeibs_ib_port *lock_ports[n_lock_ports];
	struct nvmeibs_disk_private_data *disk_private_data;
	int rv = -ENODEV;
	int i;

	NFIN;
	if (!(disks = nvmeibs_disk_get_disks(NULL))) {
		_NT(trace_disk_locks_nvmeibs_disk_locks_get_dev, "Server does not possess any disks");
		goto out;
	}

	list_for_each_entry(disk, disks, link) {
		disk_private_data = (struct nvmeibs_disk_private_data *)disk->priv;
		if (!disk_private_data) {
			_NE(error_disk_locks_nvmeibs_disk_locks_get_dev, "got null private data");
			continue;
		}
		if (strncmp(disk->disk_id, selected_disk_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE))
			continue;

		*num_gids = nvmeibs_disk_lock_get_local_dev(
			disk, lock_ports, n_lock_ports, true); /* Toma */
		if (!*num_gids) {
			_NE(error_1_disk_locks_nvmeibs_disk_locks_get_dev, "No available lock device");
			goto out;
		}

		for (i = 0; i < *num_gids; ++i)
			out_gids[i] = lock_ports[i]->gid.gid;

		rv = 0;
		break;
	}

out:
	nvmeibs_disk_put_disks();

	NFOUT;
	return rv;
}

/* per disk, put lock-dev if used by toma */
void nvmeibs_disk_locks_toma_proc_close(void)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;
	struct nvmeibs_disk_private_data *disk_private_data;
	NFIN;

	disks = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(disk, disks, link) {
		disk_private_data = (struct nvmeibs_disk_private_data *)disk->priv;
		if (!disk_private_data) {
			_NE(error_disk_locks_nvmeibs_disk_locks_toma_proc_close, "got null private data");
			continue;
		}

		nvmeibs_disk_locks_guard();
		if (disk_private_data->lock_dev_refcnt_toma)
			nvmeibs_disk_lock_dev_put_(disk, true,
				disk_private_data->lock_dev_refcnt_toma);
		nvmeibs_disk_lock_unguard();
	}
	nvmeibs_disk_put_disks();

	NFOUT;
}

int nvmeibs_disk_locks_put_dev(char *selected_disk_name)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;
	struct nvmeibs_disk_private_data *disk_private_data;
	int rv = -ENODEV;
	NFIN;

	if (!(disks = nvmeibs_disk_get_disks(NULL))) {
		_NT(trace_disk_locks_nvmeibs_disk_locks_put_dev, "Server does not possess any disks");
		goto out;
	}

	list_for_each_entry(disk, disks, link) {
		disk_private_data = (struct nvmeibs_disk_private_data *)disk->priv;
		if (!disk_private_data) {
			_NE(error_disk_locks_nvmeibs_disk_locks_put_dev, "got null private data");
			continue;
		}
		if (strncmp(disk->disk_id, selected_disk_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE))
			continue;

		nvmeibs_disk_locks_guard();
		nvmeibs_disk_lock_dev_put_(disk, true, 1);
		nvmeibs_disk_lock_unguard();
		rv = 0;
		break;
	}

out:
	nvmeibs_disk_put_disks();

	NFOUT;
	return rv;
}

void nvmeibs_disk_locks_update_mem_gids_(struct nvmeibs_dev *nis_dev,
					struct disk_mapped_mem_info *mem)
{
	struct nvmeibs_ib_port *ib_port;

	NFIN;
	mem->n_ports = 0;
	mem->node_guid = nis_dev->dev->ib_dev->node_guid;
	list_for_each_entry(ib_port, &nis_dev->port_list, port_list_n) {
		if (ib_port->gid.valid) {
			_NT(trace_disk_locks_nvmeibs_disk_locks_update_mem_gids, "mem_priv @MEM_PTR [@N_PORTS] = @GID_IPV6", mem, mem->n_ports, &ib_port->gid.gid);
			mem->ports[mem->n_ports++] = ib_port->gid.gid;
			if (mem->n_ports == MAX_HCA_PORTS) {
				_NT(trace_1_disk_locks_nvmeibs_disk_locks_update_mem_gids, "@IB_DEV_NAME has more ports @ROCE_PORTS than supported @MAX_HCA_PORTS",
					N2IB(nis_dev)->name, nis_dev->ib_ports + nis_dev->roce_ports, MAX_HCA_PORTS);
				break;
			}
		}
	}
	_NT(trace_2_disk_locks_nvmeibs_disk_locks_update_mem_gids, "Resulting number of mem->n_ports=@N_PORTS", mem->n_ports);
	NFOUT;
}

static void update_disk_dev_gids(struct nvmeibs_disk_info *disk, struct nvmeibs_dev *nis_dev)
{
	struct disk_mapped_mem_info *mem;
	bool found = false;
	NFIN;

	_NT(trace_disk_locks_update_disk_dev_gids, "Disk @DISK_ID_STR, updating gids for mem_priv of IB device @DISK_ID_NAME",
	   disk->disk_id, N2IB(nis_dev)->name);

	if (list_empty(&disk->mem_priv_list)) {
		_NE(error_disk_locks_update_disk_dev_gids, "Nothing to update");
		goto out;
	}

	list_for_each_entry(mem, &disk->mem_priv_list, link) {
		if (mem->dev == nis_dev) {
			_NT(trace_1_disk_locks_update_disk_dev_gids, "mem @MEM_PTR, dev @IB_DEV_NAME, @N_PORTS ports", mem, N2IB(mem->dev)->name, mem->n_ports);
			found = true;
			break;
		}
	}

	if (found) {
		nvmeibs_get_devices(NULL); /* Lock the devices guard */
		nvmeibs_disk_locks_update_mem_gids_(nis_dev, mem);
		nvmeibs_put_devices(); /* Unlock the devices guard */
	}
	else {
		_NT(trace_2_disk_locks_update_disk_dev_gids, "Disk @DISK_ID_STR, mem_priv not found for IB device @DISK_ID_NAME",
			disk->disk_id, N2IB(nis_dev)->name);
	}

out:
	NFOUT;
}

void nvmeibs_disk_locks_update_dev_gids(struct nvmeibs_dev *nis_dev)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *disk;
	NFIN;

	disks = nvmeibs_disk_get_disks(NULL);
	if (disks) {
		list_for_each_entry(disk, disks, link)
			update_disk_dev_gids(disk, nis_dev);
	}
	else
		_NT(trace_disk_locks_nvmeibs_disk_locks_update_dev_gids, "No disks to update");
	nvmeibs_disk_put_disks();
	NFOUT;
}

static void nvmeibs_disk_lock_dev_set_(struct nvmeibs_disk_info *di,
	struct nvmeibs_dev *lock_dev, bool is_toma)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	NFIN;

	BUG_ON(nvmeibs_use_tcp_locks);

	if (!mutex_is_locked(&locks_guard)) {
		_NE(error_disk_locks_nvmeibs_disk_lock_dev_set, "guard not taken, bail");
		BUG();
	}
	if (disk_pd->lock_dev || disk_pd->lock_dev_refcnt != 0) {
		_NE(error_1_disk_locks_nvmeibs_disk_lock_dev_set, "Disk already has lock-dev (@LOCK_DEV, refcnt=@REFCNT)",
			disk_pd->lock_dev, disk_pd->lock_dev_refcnt);
		BUG();
	}

	disk_pd->lock_dev = lock_dev;
	nvmeibs_update_lock_segments(disk_pd);
	nvmeibs_disk_lock_dev_get_(di, is_toma);

	NFOUT;
	return;
}

static void nvmeibs_disk_lock_dev_get_(struct nvmeibs_disk_info *di,
	bool is_toma)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	NFIN;

	if (!mutex_is_locked(&locks_guard)) {
		_NE(error_disk_locks_nvmeibs_disk_lock_dev_get, "guard not taken, bail");
		BUG();
	}

	if (nvmeibs_use_tcp_locks) {
		BUG_ON(disk_pd->lock_dev);
	}

	if (disk_pd->lock_dev == NULL) {
		_NE(error_1_disk_locks_nvmeibs_disk_lock_dev_get, "Disk @DISK_ID_STR, no lock-dev ", di->disk_id);
		BUG();
	}

	disk_pd->lock_dev_refcnt++;
	if (is_toma)
		disk_pd->lock_dev_refcnt_toma++;
	_NT(trace_disk_locks_nvmeibs_disk_lock_dev_get, "Disk @DISK_ID_STR, @TYPE_STR get lock-dev @IB_DEV_NAME (@LOCK_DEV), inc to @LOCK_DEV_REFCNT(@LOCK_DEV_REFCNT_TOMA)",
		di->disk_id, is_toma ? "Toma" : "Client",
		N2IB(disk_pd->lock_dev)->name, disk_pd->lock_dev,
		disk_pd->lock_dev_refcnt, disk_pd->lock_dev_refcnt_toma);

	NFOUT;
}

void nvmeibs_disk_lock_dev_put_(struct nvmeibs_disk_info *di, bool is_toma,
	int dec)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	NFIN;

	if (nvmeibs_use_tcp_locks) {
		BUG_ON(disk_pd->lock_dev);
	}
	if (disk_pd->lock_dev == NULL) {
		_NE(error_disk_locks_nvmeibs_disk_lock_dev_put, "Disk @DISK_ID_STR, no lock-dev ", di->disk_id);
		goto out;
	}
	if (disk_pd->lock_dev_refcnt < dec) {
		_NE(error_1_disk_locks_nvmeibs_disk_lock_dev_put, "Disk @DISK_ID_STR, cant dec: lock-dev @LOCK_DEV, refcnt=@REFCNT, dec=@DEC", di->disk_id,
			disk_pd->lock_dev, disk_pd->lock_dev_refcnt, dec);
		goto out;
	}
	if (is_toma && (disk_pd->lock_dev_refcnt_toma < dec)) {
		_NE(error_2_disk_locks_nvmeibs_disk_lock_dev_put, "Disk @DISK_ID_STR, Toma attempt put lock-dev it didn't get, "
		   "toma_cnt=@TOMA_CNT, dec=@DEC",
			di->disk_id, disk_pd->lock_dev_refcnt_toma, dec);
		goto out;
	}

	/* put lock-dev */
	disk_pd->lock_dev_refcnt -= dec;
	if (is_toma)
		disk_pd->lock_dev_refcnt_toma -= dec;
	_NT(trace_disk_locks_nvmeibs_disk_lock_dev_put, "Disk @DISK_ID_STR, @TYPE_STR put lock-dev @IB_DEV_NAME (@LOCK_DEV), dec to @LOCK_DEV_REFCNT(@LOCK_DEV_REFCNT_TOMA) from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		di->disk_id, is_toma ? "Toma" : "Client",
		N2IB(disk_pd->lock_dev)->name, disk_pd->lock_dev,
		disk_pd->lock_dev_refcnt, disk_pd->lock_dev_refcnt_toma,
		 __builtin_return_address(0));
	if (disk_pd->lock_dev_refcnt == 0)
		disk_pd->lock_dev = NULL;

out:
	NFOUT;
}



static const union nvmeib_lock_blkset_entry*
get_stale_entry(struct nvmeibs_disk_lock_mem_info *lmi, u64 index)
{
	void* entry;
	const u64 entry_offset = index * NVMEIB_LOCK_BLKSET_ENTRY_SIZE;
	const u64 pg_offset = entry_offset >> PAGE_SHIFT;
	if (pg_offset >= lmi->size) {
		_NE(t_01gselmi, "requested offset @PG_OFFSET_LLONG spans out of disk "
		   "lock table #pages (@SIZE_LLONG)", pg_offset, lmi->size);
		return 0;
	}

	entry = (page_address(lmi->pages[pg_offset]) + (entry_offset & (PAGE_SIZE - 1)));
	return (const union nvmeib_lock_blkset_entry*)entry;
}

static int encode_problem_report(struct nvmeibs_disk_lock_mem_info *lmi, u64 start, u64 len,
	char* buf, char* buf_end, char get_dbits, char get_stales, char get_full_val, u32* num_lbas)
{
	int i;
	const union nvmeib_lock_blkset_entry* entry;
	for (i = 0; i < len; ++i) {
		entry = get_stale_entry(lmi, start + i);
		if (!entry)
			return -ENOENT;

		WARN_ON(!get_full_val);		// Deprecated since V1.3
		if (get_dbits) {
			union nvmeib_blkset_problem_report* p = (void*)buf;
			if ((buf + sizeof(*p)) > buf_end)
				return -ENOSPC;
			++(*num_lbas);
			*p = nvmeib_blkset_problem_report_fill_new(entry, get_stales);
			buf = (char*)&p[1];
		} else if (get_stales) {
			if (entry->lock_id.bits.is_stale) {
				union nvmeib_blkset_sparse_report *p = (void*)buf;
				if ((buf + sizeof(*p)) > buf_end)
					return -ENOSPC;
				++(*num_lbas);
				p->ind = start + i;
				p->val = entry->lock_id.all;
				buf = (char*)&p[1];
			}
		} else {
			_NE(error_disk_locks_encode_problem_report, "incorrect mode setting");
			return -EINVAL;
		}
	}
	return 0;
}



// 'start' is the offset in block-set units.
// 'len' is the number of block-set dirty bits to be retrieved
int nvmeibs_disk_locks_get_ec_dirty_bytes(struct nvmeibs_disk_private_data *disk_pd,
	int seg_id, u64 start, u64 len, struct nvmeib_buffer *buf,
	char get_dbits, char get_stales, char get_full_val, char reserved,
	u32* nlbas)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	bool found = false;
	int rv = -1;
	u32 num_lbas = 0;
	struct sg_mapping_iter miter;

	NFIN;
	mutex_lock(&locks_guard);
	*nlbas = 0;

	//find lmi of the disk segment
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		found = true;
		break;
	}
	if (!found) {
		_NE(error_disk_locks_nvmeibs_disk_locks_get_ec_dirty_bytes, "lmi not found   seg_id=@SEG_ID_INT", seg_id);
		goto unlock_g;
	}

	_NT(trace_disk_locks_nvmeibs_disk_locks_get_ec_dirty_bytes, "len=@LEN_LLONG start=@START seg_id=@SEG_ID_INT", len, start, seg_id);
	(void)reserved;

	sg_miter_start(&miter, buf->sgt.sgl, buf->sgt.nents, SG_MITER_TO_SG | SG_MITER_ATOMIC);
	sg_miter_skip(&miter, buf->offset);
	while (sg_miter_next(&miter)) {
		u32 _num_lbas = 0;
		rv = encode_problem_report(lmi, start, len, miter.addr, miter.addr + miter.length,
					   get_dbits, get_stales, get_full_val, &_num_lbas);
		if (rv < 0 && rv != -ENOSPC) {
			sg_miter_stop(&miter);
			goto unlock_g;
		}
		num_lbas += _num_lbas;
		start += _num_lbas;
		len -= _num_lbas;
		if (rv >= 0)
			break;
	}
	sg_miter_stop(&miter);

	*nlbas = num_lbas;
	rv = 0;

unlock_g:
	mutex_unlock(&locks_guard);
	_NT(trace_1_disk_locks_nvmeibs_disk_locks_get_ec_dirty_bytes, "rv=@RV", rv);
	NFOUT;
	return rv;
}

static int execute_lock_cmp_n_swap_ops(struct nvmeibs_disk_lock_mem_info *lmi,
	const struct nvmeib_gen_cmd_lock_param *lparam, struct nvmeib_gen_cmd_lock_rsp *lock_rsp)
{
	int rv;

	rv = nvmeibs_disk_locks_gen_op_lock_lmi(
		lmi, lparam->op, lparam->offset, lparam->atomic.compare_add,
		lparam->atomic.compare_add_mask, lparam->atomic.swap, lparam->atomic.swap_mask,
		&lock_rsp->cmp_swap_val);

	return rv;
}

static int execute_lock_rdma_ops(struct nvmeibs_disk_lock_mem_info *lmi,
	const struct nvmeib_gen_cmd_lock_param *lparam,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp)
{
	int rv = 0;
	u64 *rdma_addr;

	if (lparam->rdma.len > NVMEIB_LOCK_DATA_SIZE) {
		_NE(execute_lock_rdma_ops_e1, "rdma data len too big @UINT > @LU",
			lparam->rdma.len, NVMEIB_LOCK_DATA_SIZE);
		rv = -1;
		goto out;
	}

	switch (lparam->rdma.table_type) {
	case NVMEIB_LOCK_TABLE_OWNER: {
		int virt_index = lparam->offset >> PAGE_SHIFT;
		int virt_entry = lparam->offset - (virt_index << PAGE_SHIFT);
		rdma_addr = (u64 *)(page_address(lmi->pages[virt_index]) + virt_entry);
	}
		break;
	default:
		_NE(execute_lock_rdma_ops_e2, "invalid table_type=@INT", lparam->rdma.table_type);
		rv = -1;
		goto out;
	}

	switch (lparam->op) {
	case NVMEIB_LOCK_RDMA_WRITE:
		memcpy(rdma_addr, lparam->rdma.data, lparam->rdma.len);
		lock_rsp->read_data = NULL;
		lock_rsp->read_len = 0;
		break;

	case NVMEIB_LOCK_RDMA_READ:
		lock_rsp->read_data = rdma_addr;
		lock_rsp->read_len = lparam->rdma.len;
		break;
	default:
		_NE(execute_lock_rdma_ops_e3, "invalid op=@INT", lparam->op);
		rv = -1;
		goto out;
	}

out:
	return rv;
}

int nvmeibs_execute_lock_gen_cmd(
	const struct nvmeib_gen_cmd_lock_param *lparam,
	struct nvmeibs_disk_private_data *disk_pd,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp)
{
	int rv;
	u64 seg_id = lparam->seg_id;
	struct nvmeibs_disk_lock_mem_info *lmi;

	//WARN_ON_ONCE(!nvmeibs_use_tcp_locks);

	lmi = nvmeibs_get_lmi(disk_pd, seg_id);
	if (!lmi) {
		_NE(nvmeibs_execute_lock_gen_cmd_e1, "failed to locate lmi seg_id=@_X", seg_id);
		rv = -ENOENT;
		goto out;
	}

	switch (lparam->op) {
	case NVMEIB_ATOMIC_CMP_AND_SWP:
	case NVMEIB_MASKED_ATOMIC_CMP_AND_SWP:
		rv = execute_lock_cmp_n_swap_ops(lmi, lparam, lock_rsp);
		break;
	case NVMEIB_LOCK_RDMA_READ:
	case NVMEIB_LOCK_RDMA_WRITE:
		rv = execute_lock_rdma_ops(lmi, lparam, lock_rsp);
		break;
	default:
		_NT(nvmeibs_execute_lock_gen_cmd_t1, "invalid lock op=@INT", lparam->op);
		rv = -EINVAL;
	}

out:
	return rv;
}

int nvmeibs_disk_locks_gen_op_lock(struct nvmeib_local_disk *disk,
	const struct nvmeib_gen_cmd_lock_param *lock_param,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp)
{
	int rv;
	struct nvmeibs_disk_info *di = disk->p;
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	NFIN;
	rv = nvmeibs_execute_lock_gen_cmd(lock_param, disk_pd, lock_rsp);
	NFOUT;
	return rv;
}

int nvmeibs_disk_locks_gen_op_lock_lmi(struct nvmeibs_disk_lock_mem_info *lmi,
									   enum nvmeib_lock_req_op op, u64 offset, u64 compare_add,
									   u64 compare_add_mask, u64 swap_val, u64 swap_mask, u64 *val)
{
	int rv = 0;
	u64 old_val_mask;
	u64 new_val_mask;
	const u64 lo32_mask = ~(u32)0;
	const u64 hi32_mask = lo32_mask << 32;
	int virt_index = offset >> PAGE_SHIFT;
	int virt_entry = offset - (virt_index << PAGE_SHIFT);
	u64 *lock_ptr = (u64 *)(page_address(lmi->pages[virt_index]) + virt_entry);

	NFIN;
	_ND(trace_disk_locks_nvmeibs_disk_locks_gen_op_lock_lmi, "lock operation on server via local_bypass: lmi=@LMI, lock_ptr @LOCK_PTR "
	   "virt_index=@VIRT_INDEX_INT, virt_entry=@VIRT_ENTRY_INT, offset=@OFFSET, compare=@COMPARE, swap=@SWAP",
		lmi, lock_ptr, virt_index, virt_entry, offset, compare_add, swap_val);
	switch (op) {
	case NVMEIB_ATOMIC_CMP_AND_SWP:
		*val = cmpxchg(lock_ptr, compare_add, swap_val);
		_ND(trace_1_disk_locks_nvmeibs_disk_locks_gen_op_lock_lmi, "cmpxchg result=@RESULT", *val);
		rv = 0;
		break;
	case NVMEIB_MASKED_ATOMIC_CMP_AND_SWP:
		*val = *lock_ptr;
		/* Check for special 32-bit cases */
		if (compare_add_mask == lo32_mask && swap_mask == lo32_mask) {
#if defined(__LITTLE_ENDIAN)
			u32 *lock_lo_ptr = (u32*)lock_ptr;
#else
			u32 *lock_lo_ptr = (u32*)lock_ptr + 1;
#endif
			/* if cmpswap succeeded we need to read the upper word again */
			*val = (u64)cmpxchg(lock_lo_ptr, (u32)compare_add, (u32)swap_val) | ((*lock_ptr) & hi32_mask);
		} else if (compare_add_mask == hi32_mask && swap_mask == hi32_mask) {
#if defined(__LITTLE_ENDIAN)
			u32 *lock_hi_ptr = (u32*)lock_ptr + 1;
#else
			u32 *lock_hi_ptr = (u32*)lock_ptr;
#endif
			/* if cmpswap succeeded we need to read the lower word again */
			*val = ((u64)cmpxchg(lock_hi_ptr, (u32)(compare_add >> 32), (u32)(swap_val >> 32)) << 32) | ((*lock_ptr) & lo32_mask);
		} else {
			/* No special case. Have to use 64-bit cmpxchg. May require multiple attempts */
			int i = 0;
			do {
				old_val_mask =
					((*val) & ~compare_add_mask) | (compare_add & compare_add_mask);
				new_val_mask = ((*val) & ~swap_mask) | (swap_val & swap_mask);
				(*val) = cmpxchg(lock_ptr, old_val_mask, new_val_mask);

			} while ((*val) != old_val_mask &&
			((*val) & compare_add_mask) == (compare_add & compare_add_mask) &&
				i++ < NVMEIBS_MAX_MASK_CMP_XCHG_ATTEMPTS);
			if (i == NVMEIBS_MAX_MASK_CMP_XCHG_ATTEMPTS) {
				_NE(error_1_disk_locks_nvmeibs_disk_locks_gen_op_lock_lmi,
					"masked cmp/swap FAILED: lmi=@LMI, lock_ptr @LOCK_PTR "
					"virt_index=@VIRT_INDEX_INT, virt_entry=@VIRT_ENTRY_INT, offset=@OFFSET,"
					" compare=@COMPARE,@ATOMIC_CMP_MASK, swap=@SWAP,@ATOMIC_SWAP_MASK after @NUM_RETRY_ATTEMPTS, old=@RESULT", lmi, lock_ptr, virt_index, virt_entry, offset, compare_add, compare_add_mask, swap_val, swap_mask, i, *lock_ptr);
				rv = -EBUSY;
			} else {
				_ND(trace_2_disk_locks_nvmeibs_disk_locks_gen_op_lock_lmi,
					"masked cmp/swap SUCCEEDED: lmi=@LMI, lock_ptr @LOCK_PTR "
					"virt_index=@VIRT_INDEX_INT, virt_entry=@VIRT_ENTRY_INT, offset=@OFFSET,"
					" compare=@COMPARE,@ATOMIC_CMP_MASK, swap=@SWAP,@ATOMIC_SWAP_MASK old=@RESULT", lmi, lock_ptr, virt_index, virt_entry, offset, compare_add, compare_add_mask, swap_val, swap_mask, *lock_ptr);
			}
		}
		break;
	default:
		_NE(error_disk_locks_nvmeibs_disk_locks_gen_op_lock_lmi, "Unsupported lock op @OP", (int)op);
		rv = -ENOTSUPP;
	}
	NFOUT;
	return rv;
}
