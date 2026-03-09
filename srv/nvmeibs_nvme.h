/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_NVME_H
#define NVMEIBS_NVME_H

#include "kr_incs.h"
#include "nvmeib_nvme.h"
#include "nvmeib_shared.h"
#include "nvmeib_utils.h"
#include "nvmeibs_main.h"
#include "nvmeibs_types.h"
#include "nvmeib_public.h"

/* all interaction with the nvme disks is perform using
   data and methods that are defined here.
*/


enum {
	NVMEIB_CSTS_SHST_MASK = 3 << 2,
};

enum {
	NVMEIB_LOG_ERROR	= 0x01,
	NVMEIB_LOG_SMART	= 0x02,
};

struct nvmeibs_disk_nvme_cmd_stats {
	u64 total_lat;
	u64 max_lat;
	u64 total_ops;
};

struct nvmeibs_disk_info {
	struct list_head link;
	u64 handle;
	/* blocks on disk */
	u64 blocks;
	/* hw blocks on disk (Used when req->use_hw_blocks = true) */
	u64 hw_blocks;
	/* disk block size - error if not 2^x <= PAGE_SIZE */
	int block_size;
	int block_shift;
	int metadata;	/* bytes per disk block */
	bool mtdt_extd;
	/* max I/O request size in blocks */
	int max_request_size;
	/* nvme drive sequence number */
	int seq;
	/* namespace id */
	u32 nsid;
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct gendisk *gendisk;
	struct nvmeibs_q_info *qs;
	/*number of rdda queues*/
	int n_qs;

	phys_addr_t msix_addr_phys;

	int alignment_size;	// Disk access alignment requiremnt in 4K blocks
	struct device_data *dev;
	struct drive_params *drv;
	struct external_drive *external;
	const char *status_str;
	/* modified under lock of disks' list guard */
	bool dying;

	void *priv;
	struct list_head mem_priv_list;

	struct nvmeib_ref nref;
	struct completion remove_done;

	struct nvmeib_ref controller_ops_ref;
	
	atomic_t register_done;

	struct {
		u64 total_ops;
		u64 total_err;
		struct nvmeibs_disk_nvme_cmd_stats read;
		struct nvmeibs_disk_nvme_cmd_stats write;
		struct nvmeibs_disk_nvme_cmd_stats discard;
	} stats;

	/* only used for printing theair values in disk's status proc */
	int max_ioqs;
	int min_local_ioqs;
	int max_local_ioqs;
	struct nvmeib_io_stats *io_stats;
	unsigned long add_jif;
};

struct nvmeibs_q_info {
	int qid;
	bool inuse;
	struct nvmeibs_disk_info *disk;
	struct nvme_completion *cq;
	dma_addr_t cq_phys;
	int cq_len;
	u32 __iomem *cq_doorbell;
	dma_addr_t cq_db_phys;
	struct nvme_command *sq;
	dma_addr_t sq_phys;
	int sq_len;
	u32 __iomem *sq_doorbell;
	dma_addr_t sq_db_phys;

	void **bb_addr_virt;
	dma_addr_t *bb_addr_nvme;
	int bb_npages;
	void *bb_mtdt_virt;
	dma_addr_t bb_mtdt_nvme;
	int bb_mtdt_nvme_len;
	u64 *prpl;
	dma_addr_t prpl_phys;
	void __iomem *msix_vec;
};

typedef void nvme_callback_t(void *arg, int status, u32 result);

struct nvme_qp_cmds_stats {
		int rd_count;
		int wr_count;
		int n_timeout_abort_cmds;
		u64 q_timeouts;
		u64 n_errors;
		u64 intr_comps_max;
		u64 intr_comps_tot;
		u64 intr_comps_cnt;
		u64 thread_comps_max;
		u64 thread_comps_tot;
		u64 thread_comps_cnt;
		u64 n_poll_loops;
		u64 n_thread_wakeups;
		u64 n_thread_sleeps;
		u64 n_spurious_intrs;
		u64 n_cq_errors;
		u64 n_dma_errors;
		u64 n_abort_cmd_failed;
		u64 n_queue_abort_cmds;
};

struct nvmeibs_nvme_req {
	struct list_head link;
	union {
		struct {
			unsigned int use_sg : 1;
			unsigned int use_hw_blocks : 1;
			unsigned int sg_md_already_mapped : 1; /* When use_sg == 1, MD was already DMA mapped to drive (address in mtdt_dma). 
								For PRPL, MD is already assumed to be mapped) */
		};
	};
	union {
		struct {
			dma_addr_t *buf_addrs;
			dma_addr_t prpl_phys;
			off_t buf_offset;
		};
		struct {
			struct sg_table table;
			struct scatterlist *sgp;
			bool sg_already_mapped;
		};
	};

	void *metadata;      /* For extended-md, expected value is NULL
							For separate-md, expected value is virt-addr of MD buffer */
	dma_addr_t mtdt_dma; /* If md_already_mapped == 0, internal use: DMA-map @metadata, else input of already mapped @metadata */
	dma_addr_t mtdt_dma_ptr;
	size_t mtdt_size;    /* if @metadata != NULL, this must be nz.
							For extended-md, expected value is 0.
							For separate-md, expected value is (@data_len >> @disk_info->block_shift) * @disk_info->metadata */

	int nvme_op;		// enum e_NVMEIB_CMD
	size_t data_len;				//# of data bytes (w/o MD)
	size_t resid_len;				//# of data bytes remaining to write
							//For trim cmd, X * sizeof(struct nvmeib_dsm_range)
	sector_t disk_block;	// In units of NVMEIBC_SECTOR_SHIFT
	nvme_callback_t *cb;
	void *arg;
	atomic_t parts_out;
	int status;
	struct work_struct work;
	struct nvmeibs_disk_info *disk_info;
	struct {
		ktime_t start_time; /*Start time of nvme command */
		bool is_recovery; /* IO for recovery */
	} stats;
	unsigned qid_hint_plus1; /* Hint for Local NVME Queue to use + 1, 0 means no hint */
	 /* Points to the nvme_qp stats that eventully processed the request cmds stats */
	struct nvme_qp_cmds_stats *nvme_qp_stats;
	/* Used for disk stats for TRIM operation (data_len == sizeof(nvmeib_dsm_range)) */
	unsigned n_dsm_lba;
#if NVMEIB_TRANSPORT_SKIP_STAGES
	/* hints nvmeibs_nr_skip_disk_access was on while we process the request */
	bool skipped;
#endif
};

int submit_local_cmd(struct nvmeibs_disk_info *d, struct nvmeibs_nvme_req *req);
struct device *get_nvme_dma_device(struct device_data *dev);
struct device *nvmeibs_disk_info_get_nvme_dma_device(struct nvmeibs_disk_info *di);
extern unsigned max_client_rsrc;

/* return numa node of a a disk*/
int nvmeibs_disk_info_numa_node(struct nvmeibs_disk_info *d);

// int nvmeibs_nvme_info(struct nvmeibs_nvme_disks_info *info);
/* set callbacks for device attributes */
/* allocate submission requests resources on a given disk */
// int nvmeibs_nvme_alloc(struct nvmeibs_nvme_allocate *alloc_req);
/* free previously allocated resources */
// int nvmeibs_nvme_free(struct nvmeibs_nvme_allocate *alloc_req);

char *nvmeibs_nvme_get_model(struct device_data *d);
char *nvmeibs_nvme_get_native_serial(struct device_data *d);
//print some disk information into a buffer.
ssize_t nvmeibs_nvme_print_qs(struct device_data *d, char *buffer, int len);
u16 nvmeibs_nvme_get_vendor(const struct nvmeibs_disk_info *info);

int nvmeibs_nvme_ioq_alloc_request(u64 disk_handle, u32 n_qs);
int nvmeibs_nvme_ioq_free(u64 disk_handle, u32 qid);
int nvmeibs_nvme_client_q_reset(struct nvmeibs_q_info *q);
int nvmeibs_nvme_client_q_destroy(struct nvmeibs_q_info *q);

struct nvmeibs_disk_report;
void set_msix_vector(
	struct nvmeibs_q_info *q, u64 msix_table_addr, u64 addr, u32 payload);
void mask_msix_vector(struct nvmeibs_q_info *q);

void nvmeibs_nvme_dump_msix_table(struct nvmeibs_disk_info *di);
void nvmeibs_remove_done(const char *id);
int nvmeibs_nvme_max_io_bb(struct device_data *d);
const char *nvmeibs_get_status(const struct nvmeibs_disk_info *di);
struct nvmeib_new_format_info;
struct nvmeib_format_disk;
int nvmeibs_nvme_format_disk(const char *disk_name,
	struct nvmeib_format_disk *fd, struct nvmeib_new_format_info *info);
struct nvmeib_disk_info;
void nvmeibs_nvme_fill_disk_info(const struct nvmeibs_disk_info *di,
	struct nvmeib_disk_info *info);
typedef void (*user_cb)(void *src, int src_len, void *priv);
int nvmeibs_nvme_identify_disk(const struct nvmeibs_disk_info *di,
	user_cb cb, void *priv);
struct nvmeibs_q_info *
nvmeibs_q_info_get_by_rsc_id(struct nvmeibs_disk_info *di, int rsc_id);

ssize_t nvmeibs_nvme_disk_qp_stats_fill(struct nvmeibs_disk_info *di,
										char *buf, int len);
void nvmeibs_nvme_disk_qp_stats_reset(struct nvmeibs_disk_info *di);
void nvmeibs_nvme_qp_stats_reset(struct nvmeibs_disk_info *di);
ssize_t nvmeibs_nvme_fill_stats_nvme_qps(struct nvmeibs_disk_info *di,
 char *buf, size_t len);
ssize_t nvmeibs_nvme_fill_qp_stats_json(struct nvmeibs_disk_info *di,
 char *buf, size_t len);
void nvmeibs_nvme_free_all_nvmeof(void);

ssize_t fill_disks(void *dummy, char *buffer, size_t len);

#endif
