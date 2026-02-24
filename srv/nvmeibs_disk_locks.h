/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/**
 * manages disk locks fuctionality
 *
 * 5/2015
 *  */


#ifndef NVMEIBS_DISK_LOCKS_H
#define NVMEIBS_DISK_LOCKS_H

#include "kr_incs.h"
#include "nvmeibs_main.h"
#include "nvmeibs_types.h"
#include "nvmeib_public.h"
#include "nvmeib_public_mmap.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_io_histograms.h"
#include "nvmeibs_serjio.h"

struct nvmeibs_disk_info;
struct nvmeibs_dev;
struct nvmeibs_io_hist_proc_ent;

/*the default size of lock set size in blocks*/
enum { NVMEIBS_DEFAULT_LOCK_SET_SIZE = 32 };

/*the default segment id*/
enum { NVMEIBS_DEFAULT_DISK_SEG_ID = 0 };


struct proc_remove {
	struct workqe_struct work;
	bool in_progress;
};

struct nvmeibs_serjio_disk_private_data;
/**
 * a context that holds the disk private data inside nvmeibs_disk_info
 */
//TODO - hide nvmeibs_disk_private_data
struct nvmeibs_disk_private_data {
	void *mem_disk_data;

	/*disk lock memories segments*/
	int n_memsegs;
	/*a list of nvmeibs_disk_lock_mem_info*/
	struct list_head lock_mems;
	/* the nic to be used for locks */
	struct nvmeibs_dev *lock_dev;
	atomic64_t disk_lock_counter;
	/* count the number of client + toma that uses lock_dev */
	int lock_dev_refcnt;
	/* Toma ref-cnt of lock-dev */
	int lock_dev_refcnt_toma;
	struct {
		/* per disk proc dir */
		struct proc_dir_entry *dir;
		/* proc file disk's status */
		struct nvmeib_public_procfs_ent *status;
		/* proc file disk's qps-stats*/
		struct nvmeib_public_procfs_ent *qps;
		/* proc file disk's iostats*/
		struct nvmeib_public_procfs_ent *iostats;
		/* proc file disk's io histogram metrics */
		struct nvmeibs_io_hist_proc_ent *io_histograms;
		/* proc file disk's nvme_qp stats */
		struct nvmeib_public_procfs_ent *nvme_qp_stats;
		/* proc file disk's qp_stats.json */
		struct nvmeib_public_procfs_ent *qp_stats_json;
	} procfs;

	/* proc file for mmap lock tables of disk to user */
	struct mmap_procfs_ent *proc_locks;
	/* proc file removal */
	struct proc_remove proc_rm;

	/* Serjio Private Data of Disk */
	struct nvmeibs_serjio_disk_private_data *serjio_pd;
	/* serjio error state */
	struct {
		unsigned no_gpt : 1;
	} serjio_err;
};

/**
 * a descriptor for a lock memory to be used by clients
 * for locking
 *
 * @param
 */
struct nvmeibs_disk_lock_mem_info {
	/*included in a list*/
	struct list_head link;
	/*unique segment id*/
	int seg_id;
	/*start address of this mem, offset in bytes from the start of the disk*/
	u64 start_addr;
	/*the size of a single lock in blocks*/
	u64 lock_set_size;
	/* size of the disk in 4kB blocks */
	u64 len;
	//u8 port_gid[16];
	//nuber of allocated pages
	u64 size;
	struct page **pages;
	/*list of locks "per devie" structs, the head device is the master*/
	int num_of_device_locks;
	struct list_head per_device_locks;
	/*the current selected device for locks*/
	struct nvmeibs_disk_lock_mem_info_for_device *selected;
};

static inline struct nvmeibs_disk_lock_mem_info *
nvmeibs_get_lmi(struct nvmeibs_disk_private_data *disk_pd, int seg_id)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		if (lmi->seg_id == seg_id) {
			goto out;
		}
	}
	lmi = NULL;

out:
	return lmi;
}

static inline struct nvmeibs_disk_lock_mem_info *
nvmeibs_verify_lmi(struct nvmeibs_disk_private_data *disk_pd, u64 lmi_val)
{
	struct nvmeibs_disk_lock_mem_info *lmi;
	list_for_each_entry(lmi, &disk_pd->lock_mems, link) {
		if ((u64)lmi == lmi_val) {
			goto out;
		}
	}
	lmi = NULL;

out:
	return lmi;
}

/**
 * memory lock information stored for device
 *
 * @author yaron (5/13/2015)
 * @param
 */
struct nvmeibs_disk_lock_mem_info_for_device {

	struct list_head entry;
	/*device that maintain the lock*/
	struct nvmeibs_dev *nic_dev;
	struct nvmeibs_disk_lock_mem_info *lmi;
	/*the mapping used for this device*/
	struct nvmeib_alloc_n_map mem;
	u32 rkey;
	u32 lkey;
};



int nvmeibs_disk_lock_disks_map_segs_for_dev(struct nvmeibs_dev *nic);
int nvmeibs_disk_lock_disk_map_segs_for_dev_(
	struct nvmeibs_disk_info *di, struct nvmeibs_dev *nic);
int nvmeibs_disk_alloc_lock_memory(struct nvmeibs_disk_info *di,
	int seg_id, u64 start, u64 lock_set_size, u64 len, u64 init_val);
void nvmeibs_disk_free_lock_memory(struct nvmeibs_disk_private_data *disk_pd);
void nvmeibs_disk_lock_unmap_on_dev(struct nvmeibs_dev *nic);



/**
 * return true if the ib port belongs to the lock device of a physical disk
 * if no lock device has been selected, the port device will be selected.
 */
bool nvmeibs_disk_locks_is_selected_device(struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port *ib_port, struct nvmeibs_client *cl);

/**
 * retrun lock device, if the device was not selected, than the first
 * valid nic will be selected
 *
 * @param disk_pd
 *
 * @return struct nvmeibs_dev*
 */
int nvmeibs_disk_lock_get_local_dev(
	struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port **lock_port,
	int n_ports,
	bool is_toma);

int nvmeibs_disk_mmap_fault(void *arg, unsigned long pg_offset,
							struct page **page);

/**
 * output the lock device of a given segment
 *
 * @param selected_disk_name the disk that contains the segment
 * @param selected_seg_id segment id in the disk
 * @param out_gid outpu gid
 *
 * @return 0 upon success
 */
int nvmeibs_disk_locks_get_dev(char *selected_disk_name, union ib_gid *out_gids,
	int *num_gids);
int nvmeibs_disk_locks_put_dev(char *selected_disk_name);

int nvmeibs_disk_locks_ldisk_alloc(struct nvmeibs_client *cl,
	struct nvmeib_local_disk *ldisk);

struct nvmeibs_disk_lock_mem_info_for_device *
nvmeibs_disk_locks_lmi_get_device(struct nvmeibs_disk_lock_mem_info *lmi,
								  u8 raw[16],
								  enum rdma_link_layer layer,
								  enum rdma_transport_type transport);

/**
 *  update the lock segments with the selected device and its memory mapping
 *  parameters
 *
 *
 * @param disk_pd - the disk private data part
 */
void nvmeibs_update_lock_segments(struct nvmeibs_disk_private_data *disk_pd);

void nvmeibs_disk_locks_guard(void);

void nvmeibs_disk_lock_unguard(void);
void nvmeibs_disk_locks_update_mem_gids_(struct nvmeibs_dev *nis_dev, struct disk_mapped_mem_info *mem);
void nvmeibs_disk_locks_update_dev_gids(struct nvmeibs_dev *nis_dev);

//called when a nic has been removed
bool nvmeibs_disk_locks_lock_dev_is_used(struct nvmeibs_dev *dev);
void nvmeibs_disk_lock_dev_put_(struct nvmeibs_disk_info *di, bool is_toma,
	int dec);
void nvmeibs_disk_locks_toma_proc_close(void);

int nvmeibs_disk_locks_get_ec_dirty_bytes(struct nvmeibs_disk_private_data *disk_pd,
		int seg_id, u64 start, u64 len, struct nvmeib_buffer *buf,
		char get_dbits, char get_stales, char get_full_val, char reserved,
		u32* nlbas);

int nvmeibs_disk_locks_gen_op_lock(struct nvmeib_local_disk *disk,
	const struct nvmeib_gen_cmd_lock_param *lock_param,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp);

int nvmeibs_disk_locks_gen_op_lock_lmi(struct nvmeibs_disk_lock_mem_info *lmi,
	   enum nvmeib_lock_req_op op, u64 offset, u64 compare_add,
	   u64 compare_add_mask, u64 swap, u64 swap_mask, u64 *val);

struct nvmeib_gen_cmd_lock_param;
int nvmeibs_execute_lock_gen_cmd(
	const struct nvmeib_gen_cmd_lock_param *lreq,
	struct nvmeibs_disk_private_data *disk_pd,
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp);

#endif //NVMEIBS_DISK_LOCKS_H
