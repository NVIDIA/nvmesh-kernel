/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_DISK_H
#define NVMEIBS_DISK_H

#include "kr_incs.h"
#include "nvmeibs_main.h"
#include "nvmeibs_types.h"
#include "nvmeibs_test.h"


struct nvmeibs_disk_prefered_port {
    /* disk prefered port for remote admin connections (arnic) */
    struct nvmeibs_ib_port *prefered_port;
    /*to be used in disk_ prefered_ports list*/
    struct list_head link;
};

int nvmeibs_disk_nvme_add_disk(struct nvmeibs_disk_info *info);
void nvmeibs_disk_nvme_remove_disk(struct nvmeibs_disk_info *info);
void nvmeibs_disk_all_free_lock_resources(void);
struct list_head* nvmeibs_disk_get_disks(int *size);
struct list_head* nvmeibs_disk_get_disks_nolock(int *size);
void nvmeibs_disk_put_disks(void);
int nvmeibs_disk_client_locate(struct nvmeibs_client *cl, const char *disk_name,
							   const char *cl_ofed_ver, const char *cl_kern_ver,
							   struct volume_server_config_locate_rsp *lrsp,
                               void *p, void *e);
int nvmeibs_disk_client_reset_io(struct nvmeibs_client *cl,
    const char *disk_name, u64 rsc_id,
    u64 msix_table_addr, u64 msix_raddr, u32 msix_payload);
struct volume_client_get_rsp;
void nvmeibs_disk_put_resources(struct nvmeibs_client *cl,
                                const char *disk_name,  struct volume_client_get_rsp *rsp);
void nvmeibs_disk_remove_client(struct nvmeibs_client *cl,
                                const char *disk_name);
/* when calling this one disk lock must be held by the caller */
void nvmeibs_disk_remove_client_all(struct nvmeibs_client *cl);

/*calculate numa distance between admin nic and a disk
for now the fuction returns 10 if the anic is on the same numa node of the
disk, otherwise 20 (this is done to be able to support numa_distance in the
future */
enum {
    //NVMEIBS_DISK_NIC_NUMA_UNREACH = 0,
    NVMEIBS_DISK_NIC_NUMA_SAME	  = 10,
    NVMEIBS_DISK_NIC_NUMA_DIFF	  = 20,
};
int nvmeibs_disk_nic_numa_dist(struct nvmeibs_disk_info *disk,
                               struct ib_device *nic_ib_dev);

/* test if given port is prefered by this disk */
bool nvmeibs_disk_is_prefered_port(struct nvmeibs_disk_info *di,
                                   struct nvmeibs_ib_port *port);

/* sets prefered port of disk*/
int nvmeibs_disk_add_preferd_port(struct nvmeibs_disk_info *di,
                                  struct nvmeibs_ib_port *port);

/* remove port from disk prefered list */
void nvmeibs_disk_remove_prefered_port(struct nvmeibs_disk_info *di,
                                       struct nvmeibs_ib_port *port);

/* retrun a list of prefered ports*/
struct list_head* nvmeibs_disk_prefered_ports(struct nvmeibs_disk_info *di);

/*print disk clients information into buffer*/
ssize_t nvmeibs_disk_print_disk_res_info(struct nvmeibs_disk_info *di,
                                         char *buffer, int len);

struct nvmeibs_q_info;
void nvmeibs_disk_nvme_ioq_alloc_done(struct nvmeibs_q_info *q);

void nvmeibs_disk_record_stats(struct nvmeibs_disk_info *di, struct nvmeibs_nvme_req *req, int status);

struct proc_dir_entry *get_disk_proc_dir(struct nvmeibs_disk_info *di);

#endif

