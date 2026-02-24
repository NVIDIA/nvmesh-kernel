/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DISK_H
#define NVMEIBC_DISK_H

#include "common/kr_incs.h"
#include "nvmeib_version_shared.h"
#include "nvmeibc_types.h"
#include "nvmeib.h"
#include "nvmeib_trend.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeib_io_stats.h"
#include "nvmeibc_admin_channel.h"
#include "nvmeibs_nvme.h"								// struct nvmeibs_nvme_req
#include "nvmeib_shared.h"
#include "nvmeibc_disk_hooks.h"
#include "nvmeibc_trend_types.h"
#include "nvmeibc_common.h"
#include "nvmeib_rdma.h"
#include "common/compat/kr_incs_compiler_types.h"
#include "nvmeibc_idisk.h"

struct nvmeibc_disk_dirty_bits_rsc {
        u32 n_pages;
        struct page **pages;
	struct sg_table sgt;
};


struct nvmeibc_disk_dirty_bits_mapping {
	struct nvmeibc_dev *nic_dev;	// device which mapped this memory

	u64 ioaddr;			// Address[bytes] relative to seg addr
	u32 length;			// Length[ bytes]
	u32 rkey;

	struct nvmeib_alloc_n_map map;
	struct list_head link;
};

/* JMDC Piggy-back Resources */
struct nvmeibc_jmdc_pb_rsrc
{
	bool mapped;
	u64 src_laddr;
	struct ib_sge src_sge[NVMEIBC_MAX_JAM_RDMA_OPS];
	struct jentry_md_container src[NVMEIBC_MAX_JAM_RDMA_OPS];
};

#define NVMEIBC_DISK_RSC_CMDID_BITS_SQE 8
#define NVMEIBC_DISK_RSC_CMDID_BITS_CNT 8
union nvmeibc_disk_channel_rsc_command_id {
	struct {
		u16 sq_entry	: NVMEIBC_DISK_RSC_CMDID_BITS_SQE;
		u16 cnt			: NVMEIBC_DISK_RSC_CMDID_BITS_CNT;
	};

	u16 all;
};

struct lock_seg_info;
struct nvmeibc_disk_info;
/* remote disk resources for a single io channel */
struct nvmeibc_disk_channel_rsc {
	u64 id;
	/* set number */
	int set_n;
	/* the bounce buffer */
	u64 mem_raddr;
	u32 mem_size;
	u32 mem_lkey;
	u32 mem_rkey;
	u32 mem_n_pages;
	dma_addr_t bb_raddr_nvme[3];
	bool has_memory;
	/* MD */
	u64 md_raddr;
	u32 md_size;
	u32 md_lkey;
	u32 md_rkey;
	/* the rdma for the MD */
	struct ib_sge md_sge;
	struct nvmeib_rdma_iu md_riu;
	/* prp1 info */
	u64 prp1_raddr;
	u32 prp1_size;
	u32 prp1_rkey;
	/* our copy of the remote pages */
	void *prp1_shadow;
	u64 prp1_shadow_dma;
	bool has_prp1;
	/* the rdma for the prp1 */
	struct ib_sge prp1_sge;
	struct nvmeib_rdma_iu prp1_riu;
	/* the submission queue prp list */
	u64 prpl_raddr;
	u32 prpl_size;
	u32 prpl_rkey;
	u32 prpl_n_pages;
	dma_addr_t *prpl_phys;
	/* our copy of the remote prpl */
	void *prpl_shadow;
	struct nvmeib_alloc_info prpl_shadow_phys;
	union {
		struct ib_pool_fmr *prpl_fmr;
		struct nvmeib_fr_desc *prpl_fr;
	};
	u64 prpl_shadow_dma;
	u32 prpl_shadow_l_key;
	bool has_prpl;
	/* the rdma for the prp list */
	struct ib_sge prpl_sge;
	struct nvmeib_rdma_iu prpl_riu;
	/* the submission queue info */
	u64 sq_raddr;
	u32 sq_size;
	u32 sq_rkey;
	u32 sq_n_entries;
	u32 sq_entry;
	/* our copy of the submission queue */
	void *sq_shadow;
	u64 sq_shadow_dma;
	/* the rdma for the submission queue */
	struct ib_sge sq_sge;
	struct nvmeib_rdma_iu sq_riu;
	/* the completion queue info */
	u64 cq_addr;
	u32 cq_size;
	u32 cq_lkey;
	u32 cq_rkey;
	u32 cq_n_entries;
	u32 cq_entry;
	/* our copy of the completion queue */
	void *cq_shadow;
	u64 cq_shadow_dma;
	/* the rdma for the completion queue */
	struct ib_sge cq_sge;
	struct nvmeib_rdma_iu cq_riu;
	/* the completion queue doorbell info */
	u64 cq_db_raddr;
	u32 cq_db_size;
	u32 cq_db_rkey;
	u32 cq_db_value;
	u16 cq_phase_bit;
	/* the rdma for the completion queue doorbell */
	struct ib_sge cq_db_sge;
	struct nvmeib_rdma_iu cq_db_riu;
	/* the doorbell info */
	u64 sq_db_raddr;
	u32 sq_db_size;
	u32 sq_db_rkey;
	u32 sq_db_value;
	/* the rdma for the doorbell */
	struct ib_sge sq_db_sge;
	struct nvmeib_rdma_iu sq_db_riu;
	/* the msi table entry */
	u64 msix_raddr;
	u32 msix_rkey;
	struct ib_sge msix_sge;
	struct nvmeib_rdma_iu msix_riu;
	struct {
		u64 addr;
		u32 payload;
		u32 ctrl;
	} msi_x_payload;

	/* read lock buffer */
	u64 read_lock_buffer[1];
	struct ib_sge rl_sge;
	struct nvmeib_rdma_iu rl_riu;
	/* the lock segment list */
	struct lock_seg_info *lsi;
	/* the current entry from the list */
	struct lock_seg_info *cur_lsi;

	/* the io channel - removed */
	union {
		void *ch;
	};

	/* admin stuff */
	struct nvmeibc_disk_info *info;
	struct list_head link;
	/* current device the resource is registered to */
	struct nvmeib_dev *nv;

	/* JMDC Piggy-back */
	struct nvmeibc_jmdc_pb_rsrc jmdc_pb;

	u64 command_cnt;
	union nvmeibc_disk_channel_rsc_command_id command_id;
	u32 cq_entry_dbg;
};

/* holds access info for remote disk segment to enable read lock piggyback
   ops on io cmds.  the only necessary params are the ioaddr, lmi and the lkey.
   the rest are there for sanity.
*/
struct lock_seg_info {
	int seg_id;
	u64 start_disk_address; /* in 4K blocks */
	u64 len; /* segment len in 4k blocks */
	u32 lockset_size; /* in 4k blocks */
	u64 ioaddr; /* the mapped address for rdma access */
	u32 rkey;
	u32 lkey;
	/* the remote side pointer for the segment lock info - used by no-RDDA*/
	u64 lmi;
};

struct hca_info {
	int n_ports;
	union ib_gid ports[MAX_HCA_PORTS];
	__be64 node_guid;
	struct nvmeibc_disk_channel_rsc *channel_rscs;
	struct lock_seg_info *lsi;
};

struct rsc_info {
	u64 id;
	struct list_head link;
};

/* Priority Queuing for pending disk commands */
enum {
	DISK_PEND_PRIO_GEN_CMDS = 0,
	DISK_PEND_PRIO_RPC_LOCKS = 0,
	DISK_PEND_PRIO_IO_START = 1,
	DISK_PEND_PRIO_IO_RECOV = 1,
	DISK_PEND_PRIO_EC_DATA_WRITE = 2,
	DISK_PEND_PRIO_EC_JRNL_WRITE = 3,
	DISK_PEND_PRIO_NON_EC_WRITE = 4,
	DISK_PEND_PRIO_OTHER_IO = 5,
	DISK_PEND_PRIO_MAX = 6,
};

struct nvmeibc_disk_pcpu_nrch {
	spinlock_t spinlock; /* Not used when in lock-less mode */
	struct list_head pending_disk_cmds;
	int n_pending;
	int max_pending;
	struct nvmeibc_ib_nordda_channel *nrch;
	u64 uid;
};

struct nvmeibc_disk_coremask_info;

/* fixed info about remote disk to create submission command */
struct nvmeibc_disk_info {
	u16 nsid;
	int sector_shift;
	u32 cntr_page_size;
	int n_rscs_sets;
	int n_disk_lock_segments;
	int n_rscs;
	int max_client_rscs;
	/* resource array */
	struct rsc_info *rscs;
	/* all resources for the disk - per remote hca */
	struct hca_info *hcaa;
	/* subset of the resources that currently used */
	struct list_head my_rscs;
	/* the size of my_rscs */
	int mine;
	/* the used part of my_rscs */
	int used;
	/* a list of channels for disk io */
	struct list_head *priority_heads[32];
	struct list_head *priority_tails[32];
	struct list_head available_norddas;

	struct list_head available_channels;
	struct list_head free_rscs;
	/* a list to hold block commands that wait for free io channel */
	struct list_head pending_disk_cmds[DISK_PEND_PRIO_MAX];
	/* total number of pending cmds in all priority lists */
	u64 tot_pending;
	/* total number of io pending cmds in all priority lists */
	u64 tot_io_pending;
	/* number pending cmds that can use nordda channel only - for debug */
	u64 n_use_nrch_only;

	/* the number of resources we are still waiting for */
	int waiting_for;
	struct completion *waiting_for_comp;

	/* the admin channel that maintains the remote disk */
	struct nvmeibc_admin_channel *ch;
	/* the owner remote disk */
	struct nvmeibc_disk *disk;
	/* the number of io ops that we performed on th disk */
	/*atomic64_t ops;*/
	u64 lock_id;

	struct nvmeibc_ib_nordda_channel *avail_nordda_for_cpu[NVMEIB_DFLT_MAX_CPUS];
	unsigned n_avail_norddas;

	struct nvmeibc_disk_pcpu_nrch pcpu_nrchs[NVMEIB_DFLT_MAX_CPUS];

	/* monotonic increasing counter - used as uid for pcpu-nrch to differ btw
	   session of same nrch (ptr) that get used by the same cpu twice in a row */
	atomic_t pcpu_nrchs_cnt;

	/* workqueue for scheduling connect and disconnect of per-cpu channels on the correct cpu */
	struct workqueue_struct *pcpu_wq;
	
	/* core mask channels */
	struct nvmeibc_disk_coremask_info *coremask_info;
};

/*
 * Disk:Toma API
 */

/* callback invoked upon receive of toma request,
 * cinst_unused - pointer to find current client instance, currently unused
 * arg - unique ID to find the appropriate struct nvmeibc_subscription_ctx*,
 * buf - struct nvmeibt_client_msg*
 */
typedef void nvmeibc_disk_toma_recv_req_callback_t(void *cinst_unused, u64 arg, u8 *buf, int len);

/* parameters for registering with TOMA */
struct nvmeibc_disk_subscription_params { 					// parameters with which client registers with (connects to) TOMA
	nvmeibc_disk_toma_recv_req_callback_t *recv_req_cb;	// Client's callback (listener) that handles messages from toma
	u64 arg;											// Arguments (segment/chunk) that was registered with toma (Tells client regarding which segment toma is sending messages)
};

/* callback invoked upon send completion */
typedef void nvmeibc_disk_toma_send_comp_callback_t(void *arg);

/* callback invoked upon receive of toma response */
typedef void nvmeibc_disk_toma_recv_rsp_callback_t(void *arg, void *toma_rsp);

/* parameters for sending toma cmd to TOMA */
/* NOTE:
   Both callback functions in this structure
   are invoked from (ib admin channel) workq
   and thus,  must be limited in duration so
   not to starve other pending work items */
struct nvmeibc_disk_toma_send_params { 					// Envelope of clients message to toma
	u8 *buf;											// Message data, starts with (struct nvmeibt_client_msg) header and optional additional info
	u32 len_toma : 16;									// Length of the message that toma sees: First len_toma bytes
	u32 len_srvr : 16;									// Length that server sees >= len_toma;
	nvmeibc_disk_toma_send_comp_callback_t *send_comp_cb;
	nvmeibc_disk_toma_recv_rsp_callback_t *recv_rsp_cb;
	void *arg;
};

/* Toma command: disk-->ib_admin_channel-->nvmeibc_toma */
struct nvmeibc_disk_toma_cmd {
	u64 handle;
	u8 type;
	union { /* future use */
		struct nvmeibc_disk_toma_send_params *send_params;
	};
};

struct nvmeibc_local_nic {
	struct nvmeibc_dev *nic_dev;
	struct list_head ports;
	struct list_head link;
	int n_ports;
	/* if true, local-nic was known to disk when it was disconnected
	   i.e. during descovery. In such case, on port-add/remove event,
	   we only need to update lport rather than add/remove the port
	   to/from disk's local-nics list. */
	bool cold_add;
};

struct nvmeibc_local_nic_port {
	struct nvmeibc_ib_port *ib_port;
	struct list_head link;
};

/**
 * The main remote disk structure is struct nvmeibc_disk.
 * nvmeibc_disk holds a list of rionics that can access it.
 * it actually holds a list a rionic wrapper as the rionic may
 * access many disks. rionic holds a list of all lionics that
 * pair with it independent of the actuall disks the lionics
 * access. the struct rionic_wrapper (that disk holds in list)
 * holds a list of the all lionics that access the disk through
 * its embedded rionic
 */

/**
 * struct rionic_wrapper: a wrapper for rionic to embed inside
 * struct nvmeibc_disk
 *
 * @author ofer (12/26/2014)
 */

struct nvmeibc_volume;
struct nvmeibc_disk_stats;

#define MAX_J2D_NIC_PORTS	(8 * MAX_HCA_PORTS)
struct nvmeibc_jour_nic_info {
	u8 gid[16];			// gid of port which mapped this memory

	u64 start;			// Address[bytes] relative to seg addr
	u32 length;			// Length[ bytes]
	u32 rkey;
};


struct dirty_bits_ctl {
	void *virt;
	size_t len;
	struct nvmeibc_disk_dirty_bits_rsc dirty_bits_mem;
	struct list_head dirty_bits_mappings;
	spinlock_t dirty_bits_spinlock;
	struct list_head dirty_bits_pending_reqs;
	int dirty_bits_already_running;
	int dirty_bits_stopping;
};

static inline void nvmeibc_disk_client_journal_mark_as_no_journal(struct nvmeibc_disk_client_journal *jour){
	jour->rng_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	jour->rng_slba = ~0;
	jour->rng_nlba = 0;
	jour->rng_nblk = 0;
	jour->rng_gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
	jour->rng_binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;
	memset(jour->serjio_boot_id, 0, NVMEIB_GID_STR_MAX);
	jour->max_rng_blk = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE;
	jour->tot_n_rng = NVMEIB_EC_MAX_JOURNAL_RANGES;
	jour->n_ents = 0;
}

struct nvmeibc_disk_contended_locks_stats {
	atomic64_t count;
	atomic64_t last_jif;
};

struct nvmeibc_disk_counters {
	atomic64_t n_warn_locate_zero_rscs;
	atomic64_t n_warn_locate_timeout;
	atomic64_t n_warn_locate_timeout_nzero_rscs;
	atomic64_t n_warn_slow_first_io_ch;
	atomic64_t n_err_rdda_read_poison;
	atomic64_t n_err_rdda_oe_max;
	atomic64_t n_err_nrch_wd_rescue;
	atomic64_t n_err_nrch_wd_rescue_comp; /* indicate missed events in poll-cq */
	atomic64_t n_err_ulp_reuse_req_timeout;
	atomic64_t n_err_core_dbgdi_detection;
	atomic64_t n_err_jam_non_free_entry_timeout;
	atomic64_t n_err_rtrn_rcook_uncomp_sends_exceeded; 	/* on ulp return rcookie, nrch-req has #uncompleted-send-comps > 1 */
	atomic64_t n_err_comp_rcook_uncomp_sends_exceeded; 	/* on wait done for send-comp of jour-write, nrch-req has #uncompleted-send-comps > 1 */
	atomic64_t n_err_send_comp_tag_vs_wc; 				/* on sedn-com sent-tag != wc->wr_id */
	atomic64_t n_err_nrch_pcpu_lookup_failed;
};

struct nvmeibc_disk_percpu_intr_stats {
	u64 total_send_intr;
	u64 total_recv_intr;
	u64 total_intr;
};

struct prefix_priority_masks_path {
	union ib_gid dgid;
	union ib_gid sgid;
	int priority;
};

enum nvmeibc_disk_local_defer_work_state {
	LOCAL_DEFER_WORK_DYING = -1,
	LOCAL_DEFER_WORK_IDLE = 0,
	LOCAL_DEFER_WORK_SCHEDULED = 1,
};

struct nvmeibc_disk {
	struct nvmeibc_idisk base; //should be the first element
	/* the disk id */
	char name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];				// like: S3HCNX0K501681.1
	char full_name[NVMEIB_HOST_NAME_LEN +
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 2];				// Exactly: disk_host-disk_name+
	char disk_host[NVMEIB_HOST_NAME_LEN];				// like: n111.acme.com,
	/*node that holds the disk*/
	char config_node_id[NVMEIB_HOST_NAME_LEN];			// like: n111.acme.com,
	/* The disk's admin nics - remote and local.
	   dup of all nics from configuration (see arnic-dup) */
	struct list_head arnics;
	//protect configuration changes list
	spinlock_t disk_conf_spinlock;
	/*indicates that a new configuration is set*/
	bool new_config_ready;
	/*the last reported node id that holds the disk*/
	char next_config_node_id[NVMEIB_HOST_NAME_LEN];
	ulong create_jiff;									// When object was created
	/*list of the last reported arnics*/
	struct list_head next_arnics;

	/* the link in the main driver disk list */
	struct list_head link;

	/* Disk's rionics.
	   The remote io nics that can access the disk and be accessed by the
	   one of the local io nics (rdda and no-rdda)- holds struct rionic.
	   This list rotates for load-balancing of RDDA channels. */
	struct list_head rionics;
	/* No-RDDA rionics load-balancing list
	   List is maintained separately from disk->rionics list so not to
	   be effected from its IO channels load-balancing (lists rotation) */
	struct list_head nr_rionics;
	/* First non-prefered no-rdda rionic */
	struct list_head *nr_np_head;
	/* main disk guard */
	spinlock_t spinlock;
	/*toma spinlock*/
	/* main volume guard */
	spinlock_t volume_spinlock;
	/* Remote disk resource info */
	struct nvmeibc_disk_info *info;
	/* 1 if we are in the process of disappearing */
	atomic_t dying;
	atomic_t shut_down_triggered;
	atomic_t paused;
	bool first_creation;
	/* stats for management */
	struct proc_dir_entry *proc_dir;
	struct nvmeib_public_procfs_ent *proc_ent_stats;
	struct nvmeib_public_procfs_ent *proc_ent_stats_json;
	struct nvmeib_public_procfs_ent *proc_ent_chstats;
	unsigned long chstats_rd;
	struct nvmeib_io_stats *stats;
	struct nvmeibc_trace_stats_scheduling trace_stats;	/* scheduling state of disk stats tracing for disk periodic timer handler */
	spinlock_t stats_spinlock;

	/* op counter */
	/* atomic64_t ops; */
	/* overeager counter */
	atomic64_t total_overeager;

	/* hold disk info for client & server running on the same machine */
	int sector_shift;
	/* metadate size [bytes] per disk block */
	int  md_size;
	bool md_extd;
	bool is_local;
	/* Access Local disk using bypass */
	bool access_local;
	/* Using server-side locks (not RDMA Atomics) */
	bool rpc_locks;

	struct nvmeib_local_disk local;
	struct nvmeib_local_server *local_server;
	/* is registered to local-srv's s_client (w/ cid = local_admin_ch->cid) */
	bool is_cl_reg;

	/* min/max no-rdda bb */
	size_t max_nordda_bb;
	size_t min_nordda_bb;
	/* max rdda bb */
	size_t max_rdda_bb;
	/* max disk dma/request size */
	int max_request_size_bytes;
	/* min/max gen cmd bb */
	size_t max_gen_cmd_bb;
	size_t min_gen_cmd_bb;

	/* used for performing config updates from main wq */
	atomic_t update_count;

	ktime_t local_sum_dt;
	u64 local_counts;

	/* work queue for channel removal works */
	struct workq_struct *remove_wq;
	/* the volumes that use this disk (list of struct nvmeibc_disk_id) */
	struct list_head volumes;

	__concurrent_access int n_ranges; /*stop_disk_io(under volume_spinlock) & nvmeibc_volume_update_volume_single_segment (without lock)
										concurrently use this variable. It feels incorrect. */

	/* wait queue to retry recovery */
	atomic_t restart_called;
	int detached;
	int rediscovery_now;
	wait_queue_head_t wait_queue;
	/* Ensures the first caller to nvmeibc_disk_start_release puts the work before another checks */
	spinlock_t restart_lock;

	struct completion *discover_comp;
	/*last discover status*/
	int discover_rv;
	unsigned long disconnect_jif;

	/* locate guard */
//	struct mutex locate_guard;
	/* holds all io_channels we are going to release due to a get request */
	struct list_head ioch_kill_list;
	/* disk pause stuff */
	atomic_t n_volumes_paused;
	atomic_t n_cont_preventors;
	bool     n_cont_prevents_waited_too_long;
	struct completion disk_paused;
	/* number of locks_channels that references used_lock_segments*/
	int set_locks_ref_count;
	/* list of used lock segments */
	struct list_head used_lock_segments;
	/* local segment locks */
	struct nvmeibc_disk_segments_locks segments_locks_local;
	/* local adming channel iff disk is local */
	struct nvmeibc_admin_channel *local_admin_ch;

	atomic_t num_contended;
	int rediscover_timeout;
	int restart_num;

	/* restart IO channel timer */
	unsigned long periodic_timer;
	struct admin_periodic periodic;
	u32 start_io_work_preempted_seqcnt;

	/* toma connection info hash table */
	struct hlist_head toma_conn_hash[TOMA_CONN_HASH_TABLE_SIZE];

	/* The following variables are used to manage pausing */
	// Daniel Todo: Change the 4 bool variables to state machine: Active, Should pause, Pausing, Paused, Offline. Remove redundant enum nvmeibc_disk_status
	__concurrent_access volatile bool should_pause; 		// Upon stop request, set should_pause and wait until the pause_preventers is 0. Transition to pausing state
	__concurrent_access volatile bool pausing;  			// In pausing state (all cpu's), when all the operations being transferred have completed, we can send back the pause callback and stop the disk
	         bool pausing_no_transfers;	// Pausing state + block layer already gave the callback (see above)
	struct disk_percpu *percpu; 		// Array with length as the number of CPU's in the system

#ifdef DEBUG_SUM
	atomic_t in_transfers;  			// Just for debug, represents the sum of 'in_transfers' of all cpu's
#endif
#ifdef DEBUG_TRANSFERS
	spinlock_t	transfer_spinlock[NVMEIB_DFLT_MAX_CPUS];		// a spinlock to guard access to the list of all IO's chained (i.e.: active) on the disk
	struct list_head transferring[NVMEIB_DFLT_MAX_CPUS];		// List of transferring objects
	u64 n_transferring[NVMEIB_DFLT_MAX_CPUS];					// Size of list. If all trasferts go into the list then == atomic_read(&in_transfers) == sum(per_cpu[i].in_transfers)
	s64 n_untracked_transferring[NVMEIB_DFLT_MAX_CPUS];		// Count of untracked objects. Signed because the dec might happen on a different cpu
#endif

#ifdef NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED
	spinlock_t	uncompleted_spinlock[NVMEIB_DFLT_MAX_CPUS];
	struct list_head uncompleted_cmds[NVMEIB_DFLT_MAX_CPUS];
	int n_uncompleted_cmds[NVMEIB_DFLT_MAX_CPUS];
#endif

	spinlock_t pause_reqs_lock; 		// Lock for the queue of requests to pause the disk.
	struct list_head pause_reqs;		// Queue: Request arrives -> Wait for preventers and IO transfers to finish -> Meanwhile, more stop request may arrived to the queue -> Respond to each reuest with an ACK -> stop the disk.

	/* hold the local nics for next discovery process.
	   populated on disk-create and updated on add/remove NIC */
	struct list_head local_nics;
	int num_lnics;

	/* proc entry for status */
	struct nvmeib_public_procfs_ent *proc_ent_status;
	struct nvmeib_public_procfs_ent *proc_ent_counters;
	struct nvmeib_public_procfs_ent *proc_ent_interrupts;
	struct nvmeib_public_procfs_ent *proc_ent_qps;
	struct nvmeib_public_procfs_ent *proc_ent_cmds;
	struct nvmeib_public_procfs_ent *unsafe_status;
	struct nvmeib_public_procfs_ent *proc_ent_status_json;
	struct nvmeib_public_procfs_ent *proc_ent_nrch_status;
	struct nvmeib_public_procfs_ent *proc_ent_ioch_status;
	struct nvmeib_public_procfs_ent *proc_ent_ioch_json;
#if defined(DISK_COUNT_REUSE) && DISK_COUNT_REUSE
	struct nvmeib_public_procfs_ent *proc_ent_reuse;
#endif
	struct nvmeib_public_procfs_ent *proc_ent_inj_err;
	struct nvmeib_public_procfs_ent *proc_ent_rediscover;
	struct nvmeib_public_procfs_ent *proc_core_masks_json;
	struct nvmeib_public_procfs_ent *proc_coremask_stats_json;

	/* Used for spdk to poll the CQs */
	struct pcpu_nrch_poll {
		int index;
		struct nvmeibc_disk *disk;
		struct proc_dir_entry *proc;
		atomic_t open_cnt;
	} pcpu_nrch_poll[NVMEIB_DFLT_MAX_CPUS];

	char inj_err;

	/* journal allocation per {client, disk} pair */
	struct nvmeibc_disk_client_journal jour;

	/* jam's info for managing @jour */
	struct nvmeibc_jam_disk *jam_disk;

	/* jam's last known state of journal range
	   - fills on disconnect-disk during nvmeibc-jam-disk-del.
	   - sent on re-connect to speedup IO resumption after disconnect.
	   - iff client (uuid) already has associated JRI and both @jrc.rng_id
	   - and @jrc.rng_genid match, serjio uses this cache (and increments
	     JGC's GID) */
	struct nvmeibc_jam_range_cache jrc;

	/* required binje to be sent on discover GET-JRANGE
	   any other logic (e.g. datapath) shall NOT use this value
	   but instead use only @disk->jour.rng_binje */
	binje_t binje_ulp;

	/* id per {client, disk} pair allocated by srv,
	   set valid admin-ch's @is_main is set */
	u32 cid;

	/* reuse channels/requests */
	atomic_t n_reused;

	/* the disk version */
	u64 version;
	/* SMP sync abstraction for disk version */
	seqcount_t version_seq;

	struct dirty_bits_ctl db;

	u32 start_ioch_ctr;

	u32 nrch_ioreq_num;

	struct workqe_struct send_io_path_ka_work;
	unsigned long last_ka_jiffies;
	atomic_t send_io_path_work_on_q;

	/* we keep these two so we can know their
	   values on crash even w/o relevant logs */
	bool allow_rediscovery;
	bool io_stopped;

	/* Disk Release Work counter.
	   Modified only when adding disk-release work i.e. atomically */
	u32 drw_cnt;

	bool tgt_ofed_kern_mismatch;
	bool tgt_no_ofed;
	char tgt_ofed_ver[NVMEIB_MAX_OFED_VER_STRLEN + 1];
	char tgt_kern_ver[NVMEIB_MAX_KERN_VER_STRLEN + 1];

	struct nvmeibc_disk_percpu_cmds_stats __percpu *pcpu_cmds_stats;
	struct nvmeibc_disk_percpu_intr_stats __percpu *pcpu_intr_stats;

	struct nvmeibc_disk_contended_locks_stats contended_locks_stats;

	struct nvmeibc_disk_counters counters;

	/* structs for testing */
	struct nvmeibc_disk_hooks *disk_hooks;				// Optional disk commands hooks
	const struct nvmeibc_cinst_params_core *cips;

	/* last target version */
	union nvmeib_version last_tgt_link_ver;
	union nvmeib_version last_tgt_ver;

	atomic64_t gen_cmds_cntrs_ok[NVMEIB_GEN_OP_MAX];
	atomic64_t gen_cmds_cntrs_fail[NVMEIB_GEN_OP_MAX];
	atomic64_t gen_cmds_cntrs_local[NVMEIB_GEN_OP_MAX];
	atomic64_t gen_cmds_cntrs_remote[NVMEIB_GEN_OP_MAX];

	struct workq_struct *local_gen_wq;
	atomic_t local_gen_wq_cnt;

	struct list_head ioch_drained_pending_list;

	/* target reported sizes over admin's login rsp, used for
	   calculating num allowed nrch-per-path and 2nd-lock-chs */
	int tgt_num_cpus;

	int tgt_max_nrchs_per_path_rdma;
	int tgt_max_nrchs_per_path_tcp;

	/* min of @tgt_num_cpus and nvmeibc_max_2nd_lock_channels */
	int max_2nd_lock_chs;

	/* for checking we run work on main admin-ch's remove-wq from
	   ctxs where it is not safe to run get_alive_admin_ch() */
	int main_ach_wq_pid;

	struct nvmeib_trend release_trend;
	struct nvmeib_trend discover_trend;
	struct nvmeib_trend peer_release_reason_trend;

	uint nr_get_by_cpu_index;
	uint prio_pending;

	/* time in jiffies where we last had 0 io chans connected */
	ulong no_io_time;
	/* number of connected io chans + no rdda chans */
	atomic_t connected_io_channels;

	bool is_tcp;

	/* can disk connect percpu nrchs */
	bool pcpu_nrchs;
	unsigned int pcpu_nrchs_max_per_disk;
	/* per-cpu nrchs are lock-less */
	bool pcpu_nrchs_ll;
	/* lock-less per-cpu nrchs cpu-mask */
	cpumask_var_t pcpu_nrchs_ll_cpumask;
	/* is coremask support enabled */
	bool coremask_support;
	/* max nrch per coremask */
	unsigned int max_coremask_nrch;

	/* does we send keep alives on no_rdda channels only for this disk */
	bool io_ka_only_no_rdda;

	/* debug mode to fail disk discover */
	bool force_pause;

	bool defer_block_cb_on_io_cmd;

	bool local_defer_block_cb_on_io_cmd;
	spinlock_t local_defer_io_lock;
	struct list_head local_defer_io_list;
	struct workqe_struct local_defer_io_work;
	struct nvmeib_state_guard local_defer_io_work_state;
	atomic_t local_defer_io_outstanding;
	struct completion *local_defer_io_comp;

	/* will be assigned by the nvmeibc_disk_pause_at_first_discover module param at nvmeibc_disk_create() */
	bool pause_at_first_discover;

	/* increments each discover call */
	int discover_id;

	spinlock_t subscribe_lock;
	int subscribe_flag;

	/* Used for new mgmt protocol */
	ulong last_target_nics_query_jif;
	ulong first_arnics_discover_fail_jif;
	ulong n_arnics_discover_fail;
	int target_nics_query_restart_num;

	struct prefix_priority_masks_path current_common_prefix_path;
	
	/* Disk Creation ID (Set from global counter in disk_create) */
	unsigned create_id;

	atomic_t deferred_io_cnt;
	wait_queue_head_t deferred_io_wait;
};

static inline struct nvmeibc_disk const* __nvmeibc_disk_from_base(struct nvmeibc_idisk const* self)
{
	BUILD_BUG_ON(offsetof(struct nvmeibc_disk, base) != 0);
	return (struct nvmeibc_disk const*)(self);
}

#define nvmeibc_disk_from_base(self) \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(self), const struct nvmeibc_idisk*),	\
		__nvmeibc_disk_from_base(self),																	\
		(struct nvmeibc_disk*)__nvmeibc_disk_from_base(self)) 											\


#if defined(__KERNEL__)
static inline void nvmeibc_disk_net_intrs_stats_inc(
		struct nvmeibc_disk *disk, bool is_send)
{
	struct nvmeibc_disk_percpu_intr_stats *p = this_cpu_ptr(disk->pcpu_intr_stats);
	++p->total_intr;
	if (is_send)
		++p->total_send_intr;
	else
		++p->total_recv_intr;
}
#endif

/* The struct below is a subset of the information (status of the disk) which is updated often and can be kept per CPU (thus removing the botleneck of attomic update from different CPU's).
 * Usage of the struct: Upon new IO request
 * 1) pause_preventers++		(Preventer is: request for lock, release lock, io command etc.)
 * 2) in_transfers++
 * 3) start async execution of the preventer (request or command is sent)
 * 4) pause_preventers--
 * 5) Request completed (activated call back of lock was aqcuired, io was written, etc)
 * 6) in_transfers--
 *
 * Disk is stopped using the following steps:
 * 1) disk->should_pause = true
 * 2) pause_preventers will never rise. New IO request's will fail syncronously (they will fluctuate around zero +1 and immediately 0 for each cpu). pause_preventers is ++ and -- on the same CPU (always!)
 * 3) Iterate over all volumes on disk and instruct them to stop issuing IO's
 * 4) Syncronously wait for pausing. Until pause_preventers == 0,  disk->pausing = true.
 * 5) Now we know that in_transfers will never rise. In transfers are ++ and -- on DIFFERENT CPU's so when there are zero transfers, the sum is zero but each CPU value can be arbitrary (negative or positive)
 * If in_transfers > 0 register callback
 * 6) The last in_transfer which terminates, calls the above callback
 * 7) The callback signifies that there is no IO on disk.
 * 8) Now Cont can arrive
 *
 * NOTE: the counters are inc/dec on the cpu that executes the request/response. as such, only the sum of all these counters (from all cpu's) has a meaning.
 * e.g.: an IO might be sent from CPU X while its response processed on CPU Y. so X is incremented while Y is decremented. hence, counters gradually drift away from 0 as the load of send/complete is not equally handled by all cpu's.
 * also note that since the counters are 32b, they will wrap-around pretty fast. however, since we only care whether the sum is 0 or not, this doesnt matter.
 *
 * The pause layer is invoked twice from transport:
 * first, in interrupt ctx, to ensure it stops dispatching IO's to disk ASAP.
 * second is from the disk wq. this will either ACK the PAUSE request immediately (if no IO is in flight) or attach the request to the disk, so that the last IO to complete invokes the ACK.
 */
struct disk_percpu {
	volatile int pause_preventers;  	// Amount of todo IO request (not started yet) preventing from pause/unregistration of the disk, 0 or 1
	volatile int in_transfers;  		// Amount of currently running IO' reqs. May be arbitrary number on each cpu but the sum is always >= 0 and represents the amount of running stuff.
};

/* When a disk fails or disconnects, we must notify the upper layer (block device). We do it by sending a pause request to all block devices 'registered' for this disk.
 * The pause function of block device returns after it can guarantee that no new requests will be made to this disk.
 * Later, a callback is sent to us when the block device has completed receiving the result of all outstanding requests to this disk.*/
struct pause_req {
	void *cb;   						// Call back to execute as response to a request (once the disk was paused/stopped)
	void *cntx; 						// Parameters to the callback
	struct list_head reqs;  			// Pointer to next request in the linked list of requests
};

/* create remote disk assets */
struct nvmeibc_ib_admin_channel;
struct nvmeibc_block_device;
/* create a new disk. positive value means that discover is not finished,
   and the calles must wait, and collect the result of discover*/
int nvmeibc_disk_create(const struct nvmeibc_cinst_params_core *p,
		struct nvmeibc_disk_id *disk_id, struct list_head *arnics,
						int num_ranges, const char *node_id);
/*block- wait to the end of disk creation (that is, discover)*/
int nvmeibc_disk_wait_for_discover(struct nvmeibc_disk_id *current_disk);
void nvmeibc_disk_add_volume(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_id *disk_id, int num_ranges);
/*set new configuration for the disk*/
int nvmeibc_disk_set_next_config(struct nvmeibc_disk_id *disk_id,
	const char *node_id);
int nvmeibc_disk_create_remote(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, int nsid, int sector_shift, int p, int n, int m,
	u64 lock_counter, int n_disk_lock_segments, __be64 *rsrc_guids, bool is_rediscover);
bool nvmeibc_disk_remove(struct nvmeibc_disk_id *disk_id, bool block);
/*block till the end of disk disk release*/
void nvmeibc_disk_block_remove(struct nvmeibc_disk_id *disk);

struct nvmeibs_disk_description;
int nvmeibc_disk_add_rsc(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, struct nvmeibs_disk_description *d,
	int j, int p);
struct nvmeibs_disk_seg_lock_info;
int nvmeibc_disk_add_lock_rsc(struct nvmeibc_ib_admin_channel *ach,
	struct nvmeibc_disk *disk, struct nvmeibs_disk_seg_lock_info *e,
	int j, int p);
int nvmeibc_disk_start_release(struct nvmeibc_disk *disk, enum nvmeibc_disk_release_reason reason);
int nvmeibc_disk_locate_resource(struct nvmeibc_disk *disk, u64 id);
int nvmeibc_disk_start_io_channels(struct nvmeibc_disk *disk, bool high_pri);
void nvmeibc_disk_start_io_channels_(struct nvmeibc_disk *disk);
struct volume_server_req;
struct volume_client_rsp;
int nvmeibc_disk_handle_controller_req(struct nvmeibc_disk *disk,
	struct volume_server_req *req, struct volume_client_rsp *rsp,
	int *rsp_len);

struct nvmeibc_disk_io_command;
int nvmeibc_disk_execute_io(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *block_cmd);
struct nvmeibc_lock_opr_in_progress;
struct nvmeibc_disk_lock_cmd;
int nvmeibc_disk_execute_lock(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_lock_cmd *lock_cmd);
void nvmeibc_disk_reused_bb_release(struct nvmeibc_disk *disk,
	struct nvmeib_data_reuse_buf_params* p);	// If command saved a buffer but next command does not need to reuse it, can locally clean it

struct nvmeibc_disk_gen_cmd;
int nvmeibc_disk_execute_gen(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_gen_cmd *gen_cmd);
struct nvmeibc_disk_io_command * nvmeibc_disk_get_block_cmd_rdda(
	struct nvmeibc_disk *disk, struct nvmeibc_channel *ch, u32 version);
struct nvmeibc_volume_req_info;
struct nvmeibc_disk_command *nvmeibc_disk_get_disk_cmd_nordda(
	struct nvmeibc_disk *disk, struct nvmeibc_ib_nordda_channel *ch,
	u64 version, struct nvmeibc_volume_req_info *req,
	void (*put_req_fn)(struct nvmeibc_ib_nordda_channel *ch, struct nvmeibc_volume_req_info *req));
struct nvmeibc_disk_command *nvmeibc_disk_pcpu_nrch_pending_cmd_get(
	struct nvmeibc_disk *disk, struct nvmeibc_ib_nordda_channel *ch,
	u64 version, struct nvmeibc_volume_req_info *req,
	void (*put_req_fn)(struct nvmeibc_ib_nordda_channel *ch, struct nvmeibc_volume_req_info *req));
void nvmeibc_disk_init_stats(struct nvmeibc_disk *disk);
void nvmeibc_disk_print_stats(struct nvmeibc_disk *disk, const char *str);

/* A struct for software disk I/O requst. Daniel todo: Missleading name. This io_req has nothing to do with block device */
struct nvmeibc_block_io_req {
    int /* enum nvmeib_block_io_op*/ op;    // Type of the operation. Read/Write/Trim.
	u8 do_512b_sub_block_x;					// Allow Read/Write sub block
#ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
    bool ndb_mapped;						// Used by lower layer modules: ndb has been mapped to RDMA
#endif
	u64 disk_address;                       // The remote disk address (in units of volume blocks = 1<<NVMEIBC_SECTOR_SHIFT)
    struct nvmeib_data_buffer *ndb;         // Todo: Embed inside to save 8bytes. Holds information with regards to the memory mapping of the data of the request. S/G table
    struct nvmeibs_nvme_req req;            // The nvme request for local usage
	union {
		void *md;        					// Array of per-block meta-data (num elements ==  blocks in ndb). Each element of size nvmeibc_disk_sw_md_size(), Irrelevant for Trim; see NVMEIBC_D2MD_LEN
		u64  *rmw_md_act;					// Alternative to above, in read_modify_write stores the action (u64) to apply to metadata
	};
	union {
		struct nvmeibc_jam_rdma_op  jam_op;	// Optional Journal ram operation. Piggybacked on journal IO command
		struct {
			u64 dont_touch_must_be_0_due_to_union;
			struct nvmeib_dsm_range *trim;	// Used for discard can't be vmem since it's used for DMA.
		};
	};
	const struct nvmeib_cpu_mask_info *cpu_mask_info;
	int submit_cpu;


#ifdef DEBUG_UNCOMPLETED
	u64 nlbas;                          	// ???
#endif // DEBUG_UNCOMPLETED
};

/* Data-length to Metadata-length */
#define NVMEIBC_D2MD_LEN(_dlen_, _disk_) (NVMEIB_D2MD_LEN(_dlen_, _disk_->sector_shift, _disk_->md_size))
#define NVMEIBC_DCMD_LEN_TO_SW_SECTORS(_dcmd_) (((const struct nvmeibc_disk_io_command *)(_dcmd_))->reqs[0].ndb->length >> NVMEIBC_SECTOR_SHIFT)

void nvmeibc_disk_add_stats(struct nvmeibc_disk *disk, 
			struct nvmeibc_dev *local_dev,
			struct nvmeib_io_stats *stats,
			struct nvmeibc_block_io_req *req, u64 io_exec,
			bool is_recovery);

int nvmeibc_disk_add_work(struct nvmeibc_disk *disk,
	struct workqe_struct *work);

int nvmeibc_disk_add_admin_work(struct nvmeibc_disk *disk, struct workqe_struct *work);

void nvmeibc_disk_pause(struct nvmeibc_disk *disk);

int nvmeibc_disk_extract_piggyback_lock_info(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *iocmd, struct lock_seg_info *lsia,
	enum nvmeibc_disk_locks_opr opr_type, struct lock_seg_info **lsi, u64 *offset);

/* Disk:Toma API function */
/* Called once when block starts */
int nvmeibc_disk_subscribe_toma_service(struct nvmeibc_disk *disk, u64 handle,
	struct nvmeibc_disk_subscription_params *params);
/* Called once when block dies */
int nvmeibc_disk_unsubscribe_toma_service(struct nvmeibc_disk *disk, u64 handle);
int nvmeibc_disk_toma_send(struct nvmeibc_disk *disk, u64 handle,
						   struct nvmeibc_disk_toma_send_params *params);
struct nvmeibc_toma_recv_msg;
void nvmeibc_disk_toma_recv(struct nvmeibc_disk *disk,
							struct nvmeibc_toma_recv_msg *toma_recv_msg);

void nvmeibc_disk_call_discover(struct nvmeibc_disk *disk);

static inline enum nvmeibc_disk_status __nvmeibc_disk_get_status_impl(const struct nvmeibc_idisk *self)
{
	struct nvmeibc_disk const* disk = nvmeibc_disk_from_base(self);
	return (disk && !atomic_read(&disk->paused)) ? d_online : d_offline;
}

static inline char const *__nvmeibc_disk_get_name_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->name;
}

static inline char const *__nvmeibc_disk_get_full_name_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->full_name;
}

static inline char const *__nvmeibc_disk_get_host_name_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->disk_host;
}

static inline bool __nvmeibc_disk_should_pause_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->should_pause;
}

static inline bool __nvmeibc_disk_is_cont_preventors_waited_too_long_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->n_cont_prevents_waited_too_long;
}

static inline int __nvmeibc_disk_get_sector_shift_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->sector_shift;
}

static inline int __nvmeibc_disk_get_md_size_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->md_size;
}

static inline int __nvmeibc_disk_get_max_request_size_bytes_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->max_request_size_bytes;
}

static inline bool __nvmeibc_disk_is_access_local_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->access_local;
}

static inline size_t __nvmeibc_disk_get_min_gen_cmd_bb_impl(struct nvmeibc_idisk const *self)
{
	return nvmeibc_disk_from_base(self)->min_gen_cmd_bb;
}

static inline struct nvmeibc_disk_client_journal const *__nvmeibc_disk_get_journal_impl(struct nvmeibc_idisk const *self)
{
	return &nvmeibc_disk_from_base(self)->jour;
}

//for tests, will be removed in future
static inline struct nvmeibc_disk_client_journal *__nvmeibc_disk_get_journal_mut_impl(struct nvmeibc_idisk *self)
{
	return &nvmeibc_disk_from_base(self)->jour;
}

static inline int __nvmeibc_disk_read_cont_preventors_impl(struct nvmeibc_idisk *self)
{
	return atomic_read(&nvmeibc_disk_from_base(self)->n_cont_preventors);
}

static inline int __nvmeibc_disk_inc_cont_preventors_impl(struct nvmeibc_idisk *self)
{
	return atomic_inc_return(&nvmeibc_disk_from_base(self)->n_cont_preventors);
}

static inline int __nvmeibc_disk_dec_cont_preventors_impl(struct nvmeibc_idisk *self)
{
	return atomic_dec_return(&nvmeibc_disk_from_base(self)->n_cont_preventors);
}

static inline void __nvmeibc_disk_call_discover_impl(struct nvmeibc_idisk *self)
{
	nvmeibc_disk_call_discover(nvmeibc_disk_from_base(self));
}

//the accessors above are temporal only, until block kernel simulator will implement his private version of the disk
static inline void nvmeibc_disk_base_init(struct nvmeibc_disk *self)
{
	self->base.ops.get_name = __nvmeibc_disk_get_name_impl;
	self->base.ops.get_full_name = __nvmeibc_disk_get_full_name_impl;
	self->base.ops.get_host_name = __nvmeibc_disk_get_host_name_impl;
	self->base.ops.should_pause = __nvmeibc_disk_should_pause_impl;
	self->base.ops.is_cont_preventors_waited_too_long = __nvmeibc_disk_is_cont_preventors_waited_too_long_impl;
	self->base.ops.get_sector_shift = __nvmeibc_disk_get_sector_shift_impl;
	self->base.ops.get_md_size = __nvmeibc_disk_get_md_size_impl;
	self->base.ops.get_max_request_size_bytes = __nvmeibc_disk_get_max_request_size_bytes_impl;
	self->base.ops.is_access_local = __nvmeibc_disk_is_access_local_impl;
	self->base.ops.get_min_gen_cmd_bb = __nvmeibc_disk_get_min_gen_cmd_bb_impl;
	self->base.ops.get_journal = __nvmeibc_disk_get_journal_impl;
	self->base.ops.get_journal_mut = __nvmeibc_disk_get_journal_mut_impl;
	self->base.ops.read_cont_preventors = __nvmeibc_disk_read_cont_preventors_impl;
	self->base.ops.inc_cont_preventors = __nvmeibc_disk_inc_cont_preventors_impl;
	self->base.ops.dec_cont_preventors = __nvmeibc_disk_dec_cont_preventors_impl;
	self->base.ops.get_status = __nvmeibc_disk_get_status_impl;
	self->base.ops.call_discover = __nvmeibc_disk_call_discover_impl;
}


struct nvmeibc_disk_get_segs_locks_flags {
	unsigned write:1;
	unsigned dont_wait:1;
	unsigned local_only:1;
	unsigned remote_only:1;
};

struct nvmeibc_disk_segments_locks *nvmeibc_disk_get_segs_locks(
	struct nvmeibc_disk *disk, struct nvmeibc_disk_get_segs_locks_flags get_segs_locks_flags);

void nvmeibc_disk_put_segs_locks(struct nvmeibc_disk_segments_locks *seg_locks,
								 struct nvmeibc_disk_get_segs_locks_flags get_segs_locks_flags);

void nvmeibc_disk_volumes_get(struct nvmeibc_disk *disk, unsigned long *flags);
void nvmeibc_disk_volumes_put(struct nvmeibc_disk *disk, unsigned long *flags);
struct nvmeibc_block_device;
void nvmeibc_disk_block_ready_nolock(
	struct nvmeibc_disk *disk, const struct nvmeibc_block_device *dev);
void nvmeibc_disk_print_ch_info(struct nvmeibc_disk *disk);

/**
 * clear unsuded admins
 *
 * @param disk
 */
void nvmeibc_disk_free_unused_admin_ch(struct nvmeibc_disk *disk);

int nvmeibc_disk_find_path(struct nvmeibc_disk *disk,
						   struct nvmeib_rdma_path_info *info);

/* updates performed from main wq */
enum nvmeibc_disk_update_type {
	DISK_NO_UPDATE = 0,
	DISK_UPDATE_LOCAL_SRV,
	DISK_UPDATE_REMOVE_NIC,
	DISK_UPDATE_ADD_NIC,
	DISK_UPDATE_REMOVE_PORT,
	DISK_UPDATE_ADD_PORT,
	DISK_UPDATE_PORT_UPDATE,
	DISK_UPDATE_WRITE_STATUS,
	DISK_UPDATE_JAM_ABND2FREE,
	DISK_UPDATE_REMOTE_GID,
	DISK_UPDATE_DISCONNECT_IO_PATH,
	DISK_UPDATE_RESET_QP_STATS,
	DISK_UPDATE_RESET_COREMASK_STATS,
	DISK_UPDATE_COREMASK_UPDATE,
};

const char *nvmeibc_disk_update_type_str(enum nvmeibc_disk_update_type update_type);

typedef void (*nvmeibc_disk_update_done_cb)(void *);

struct nvmeibc_disk_update_data {
	enum nvmeibc_disk_update_type update_type;
	void *update_data;
	nvmeibc_disk_update_done_cb done_cb;
	void *done_cb_ctx;
};

int nvmeibc_disk_update_config(struct nvmeibc_disk *disk,
							   struct nvmeibc_disk_update_data *update_data,
							   bool in_interrupt);

ssize_t nvmeibc_disk_print_info(struct nvmeibc_disk *disk, char *buffer,
	int len);
void nvmeibc_disk_available_norddas_del(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_nordda_channel *nrch);

struct nvmeibc_disk_jmdc_read_comp;
int nvmeibc_disk_jmdc_read(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp);

int nvmeibc_disk_dbg_please_kill_yourself(struct nvmeibc_disk *disk,
                                          void (*cb)(void *), void *ctx,
                                          int rsc_id, u64 dlba);

struct nvmeibc_blkset_recovered_rsp {
	/*  0 			: Found jmdc and and cleaned the/an entry
	 *  -ENOENT		: Found no jmdc or no such entry; allowed
	 *   		  	  only if requested did not specify jidx.
	 *  Otherwise 	: Any other error, failure.
	 */
	u32 status;
};

/* Disk Generic Command
 * Usage:
 * 1. RDMA-Write to pre-mapped area at Target from @addr.
 *    The mapping is done per nrch of disk, upon connect.
 *    The net-layer finds this mapping base on @opcode
 *    and verify that @offset + @length are in-range.
 *
 * 2. Request Target to RDMA-Write to @addr and send Rsp.
 *    net-layer maps @addr to NIC until command completes
 *
 * 3. Expand...
 */

struct nvmeibc_disk_gen_cmd_data_sink {
	union {
		struct ib_pool_fmr **fmr;
		struct nvmeib_fr_desc **fr;
	};
	struct nvmeib_mr_info mri;
	struct nvmeib_gen_cmd_data *data;
};

struct nvmeibc_disk_gen_cmd {
	struct nvmeibc_disk_command disk_cmd;
	enum nvmeib_gen_cmd_op opcode;
	u64 req_id; /* OL: use it, generalize the req_id_counter impl */

	/* Array of ptrs to data sinks for Use-Case 2 */
	struct nvmeib_gen_cmd_data *data_sink[NVMEIB_MAX_GEN_CMD_DATA_SINK];
	unsigned n_data_sink;

	/* In case of Use-Case 2. Source data to write to bounce-buffer before running server-side cmd */
	struct nvmeib_data_buffer src_ndb;
	void *src;
	u32 src_off;
	u32 src_len;

	/* completion */
	void *ctx;
	int comp_code;

	/* cmd specific params */
	struct nvmeib_gen_cmd_param param;
	/* cmd specific response */
	union nvmeib_gen_cmd_rsp rsp;

	/* internal usage */
	unsigned long jiffies_start;
	unsigned long timeout;
	/* iostats */
	ktime_t send_time;
	ktime_t recv_comp_time;
	size_t recv_sz;
	/* For local bypass */
	struct nvmeibc_disk *disk;
	bool local_bypass; /* Allow GEN cmds to be sent either local or remote on an as-need basis */
	struct workqe_struct work;
	struct nvmeib_cpu_mask_info cpu_mask_info;
};

static inline struct nvmeibc_disk_gen_cmd * disk_to_gen(
	struct nvmeibc_disk_command *disk_cmd)
{
	return container_of(disk_cmd, struct nvmeibc_disk_gen_cmd, disk_cmd);
}

struct nvmeibc_disk_lock_cmd {
	struct nvmeibc_disk_command disk_cmd;
	struct nvmeibc_locks_channel *ch;
	int index;
	struct nvmeib_gen_cmd_lock_param lock_param;
	struct nvmeib_gen_cmd_lock_rsp lock_rsp;
	/* For lock bypass */
	struct nvmeibc_disk *disk;
	struct workqe_struct work;
};

static inline struct nvmeibc_disk_lock_cmd * disk_to_lock(
	struct nvmeibc_disk_command *disk_cmd)
{
	return container_of(disk_cmd, struct nvmeibc_disk_lock_cmd, disk_cmd);
}

#define NVMEIBC_DISK_CMD_COMP_CODE_INVALID (0xeeeecccc)

/*************************** Cold Recovery ************************************/

struct nvmeibc_disk_jmdc_read_comp {
	struct nvmeibc_disk *disk;			// Stores on which disk the read is performed
	u16 start_rng;
	u16 num_rng;
	bool dirty_only;
	bool client_uuid_only;
	enum nvmeib_recov_src recov_src;
	char seg_uuid[NVMEIB_GID_STR_MAX]; 	/* Unique ID of segment, allocated by management */
	uuid_be client_uuid; /* When client_uuid_only is set */

	/*if any of these are NULL, then only the lengths are retrieved */
	struct nvmeib_alloc_info *rng_data_ai; /* Buffer allocated for the range data (array of nvmeib_get_jmdc_rng_data) */
	struct nvmeib_alloc_info *ent_md_ai;	/* Buffer allocated for entries metadata (array of nvmeib_get_jmdc_ent_md) */
	struct nvmeib_alloc_info *jmdc_ent_ai; /* Buffer allocated for the jmdc entries (array of jblock_md) */
	struct nvmeibc_disk_jmdc_read_rsp {
		struct nvmeib_jmdc_read_jrnl_data *read_jrnl_data;
		struct nvmeib_get_jmdc_rng_data *rng_data;
		size_t *rng_data_len;
		struct nvmeib_jrnl_ent_md *ent_md;
		size_t *ent_md_len;
		union jblock_md *jmdc_ent;
		size_t *jmdc_ent_len;
		enum nvmeibc_block_lock_status status;	// RV of RDMA request: Important values: TAKEN = Success, FAIL_NO_COMP, FAIL_COMP, DISK_DEAD
	} rsp;
	void (*callback)(struct nvmeibc_disk_jmdc_read_comp *comp);
	#ifdef DEBUG_TRANSFERS
		struct nvmeibc_transfer_reason reason;	// Who issued this disk transfer, used to detect which transfer hasn't finished and prevents disk PAUSE
		bool in_flight;
	#endif
};

struct nvmeibc_disk_free_jrnl_ents_comp {		// Command to serjio to free journal entries
	struct nvmeibc_disk *disk;
	struct nvmeibc_disk_gen_cmd *gen_cmd;

	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	char seg_uuid[NVMEIB_GID_STR_MAX]; 	/* Unique ID of segment, allocated by management */
	enum nvmeib_recov_src recov_src;
	/* Used for HTR */
	u64 blkset_slba;
	bool pass2toma;
	u64 blkset_num;
	u64 lock_ent;

	/* Number of entries */
	unsigned start_ent;
	unsigned num_ents;
	/* Array of entries to free */
	struct nvmeib_free_ents_data *ents;
	size_t ents_sz;
	/* Buffer to encode ents into (size of max_gen_cmd_bb) */
	size_t ents_enc_buf_sz;
	void *ents_enc_buf;
	struct nvmeib_alloc_info ents_enc_ai;
	enum nvmeibc_block_lock_status status;	// RV of RDMA request: Important values: TAKEN = Success, FAIL_NO_COMP, FAIL_COMP, DISK_DEAD
	void (*callback)(struct nvmeibc_disk_free_jrnl_ents_comp *comp);
	#ifdef DEBUG_TRANSFERS
		struct nvmeibc_transfer_reason reason;	// Who issued this disk transfer, used to detect which transfer hasn't finished and prevents disk PAUSE
		bool in_flight;
	#endif
};

int nvmeibc_disk_free_jrnl_ents(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_free_jrnl_ents_comp *comp);

void nvmeibc_disk_gen_cmd_completion(
	struct nvmeibc_disk_gen_cmd *gen_cmd, int comp_code);
void nvmeibc_disk_gen_cmd_completion__(struct nvmeibc_disk_gen_cmd *gen_cmd);
bool nvmeibc_disk_gen_cmd_is_timed_out(
	const struct nvmeibc_disk_gen_cmd *gen_cmd, ulong now);
#if defined(DISK_COUNT_REUSE) && DISK_COUNT_REUSE
#	define nvmeibc_disk_inc_reuse(disk) atomic_inc(&disk->n_reused)
#	define nvmeibc_disk_dec_reuse(disk) atomic_dec(&disk->n_reused)
#else
#	define nvmeibc_disk_inc_reuse(disk) do {} while(0)
#	define nvmeibc_disk_dec_reuse(disk) do {} while(0)
#endif

u64 nvmeibc_disk_version_get(struct nvmeibc_disk *disk);
struct nvmeibc_channel;

struct lock_seg_info * nvmeibc_disk_get_lock_seg_info(
	struct nvmeibc_disk *disk, struct lock_seg_info *lsia, int seg_id);

/* not in use
static inline int __nvmeibc_disk_hw_md_size(struct nvmeibc_disk *disk)
{
	return disk->md_size;
}
*/

/* Segment sw_md_size shall be taken from disk.
   There is a corner case though, if the disk is dead, md may be 0
   even if it is not.
   For EC praids, it is a problem as we later rely on metadata non 0.
   This is why, in case md_size is 0, and it is ec, we will put md = 8
   anyway. The assumption is that as soon as disk goes up, and discovery
   runs, new topology size will arive, and we will set correct md_size
   there.

	BOTTOM LINE: in most cases you should query the topology(segment) for the sw_md_size
	It will stay unchaged as long as topology leaving;
*/
static inline int __nvmeibc_disk_sw_md_size(struct nvmeibc_disk *disk)
{
	return disk->md_size << (NVMEIBC_SECTOR_SHIFT - disk->sector_shift);
}

enum nvmeibc_arnic_attrib {
	NVMEIBC_ARNIC_ALIVE = (1 << 0),
	NVMEIBC_ARNIC_HAS_CH = (1 << 1),
	NVMEIBC_ARNIC_HAS_MAIN_CH = (1 << 2),
	NVMEIBC_ARNIC_HAS_RIONICS = (1 << 3),
};

bool nvmeibc_disk_use_arnic_for_disk(struct nvmeibc_admin_rnic *arnic,
	struct nvmeibc_disk *disk, int attrib_mask);

struct nvmeibc_io_lnic *nvmeibc_disk_create_lionic(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_port *port, struct nvmeib_rdma_path_info *info);

int nvmeibc_disk_handle_rgid_change(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_admin_channel *ch, struct volume_server_req *req);
void nvmeibc_disk_handle_rgid_change_work(struct workqe_struct *work);

int nvmeibc_disk_lionic_rionic_find_path(struct nvmeibc_io_lnic *lionic);

int nvmeibc_disk_disconnect_io_path(struct nvmeibc_disk *disk, union ib_gid *lgid, union ib_gid *rgid);

int nvmeibc_disk_handle_ioch_drained(struct nvmeibc_disk *disk,
	struct nvmeibc_ib_admin_channel *ach, struct volume_server_req *req);

bool nvmeibc_disk_ioch_drained_is_pending(struct nvmeibc_channel *ch);
bool nvmeibc_disk_ioch_drained_is_empty(struct nvmeibc_channel *ch);
void nvmeibc_disk_ioch_handoff_bailed_cmds(struct nvmeibc_channel *ch,
										   struct list_head *list);

void nvmeibc_disk_channel_version_invalidate(struct nvmeibc_channel *ch);

void nvmeibc_disk_connect_nrch_pcpu_work(struct work_struct* work);

struct nvmeibc_disk_cmd_stats {
	/* upper layer interface */
	u64 n_ulp_submissions;
	u64 n_ulp_completions;
	/* internal & low layer */
	u64 n_direct_tot;
	u64 n_pending_tot;
	int n_pending_now;	// (inc'ed & dec'ed)
	int n_executing; 	// (inc'ed & dec'ed)
	u64 n_llp_completions;
	/* posion check */
	u64 n_llp_poison_check_hits;
	u64 n_llp_poison_check_misses;
	/* errors */
	u64 n_err_internal_retry;
	u64 n_err_direct_exec;
	u64 n_err_pending_exec;
	u64 n_err_pending_timeout;
	/* from prev discovery */
	u64 n_err_pending_aborted;
};

struct nvmeibc_disk_percpu_cmds_stats {
	struct nvmeibc_disk_cmd_stats io;
	struct nvmeibc_disk_cmd_stats gen;
	struct nvmeibc_disk_cmd_stats lock;
	struct nvmeibc_disk_cmd_stats lock_hw_owner;
	struct nvmeibc_disk_cmd_stats lock_hw_active;
	struct nvmeibc_disk_cmd_stats lock_hw_wbinfo;
	struct nvmeibc_disk_cmd_stats lock_hw_read;
};

/* There is a race condition if 2 threads are trying to write last_jif at the same time,
   but this is not a real problem, since this means they are writing the same value. */
#define nvmeibc_disk_report_long_contended_lock(__disk)                   \
	({                                                                    \
		atomic64_inc(&(__disk)->contended_locks_stats.count);             \
		atomic64_set(&(__disk)->contended_locks_stats.last_jif, jiffies); \
	})

#define nvmeibc_disk_counters_inc(__disk, __counter) \
	({ atomic64_inc(&(__disk)->counters.__counter); })


#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)

#ifndef NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT
#define NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT 0
#endif

#define nvmeibc_disk_cmds_stats_init_lock_cmd(_x_, _comp)					\
do { 																	\
	switch (_comp->opr) {												\
	case NVMEIBC_LOCK_CMP_AND_SWAP: 									\
		_comp->lock_cmd.cmd_type = NVMEIBC_DISK_CMD_LOCK_HW_OWNER;		\
		break; 															\
	case NVMEIBC_LOCK_BLKSET_INFO_WRITE: 								\
		_comp->lock_cmd.cmd_type = NVMEIBC_DISK_CMD_LOCK_HW_WBINFO;		\
		break; 															\
	case NVMEIBC_LOCK_READ:												\
		_comp->lock_cmd.cmd_type = NVMEIBC_DISK_CMD_LOCK_HW_READ;		\
		break; 															\
	default:															\
		_NE(_x_, "Lock-stats, unsupported opr=@INT", _comp->opr); 	 	\
		break;															\
	}																	\
	nvmeibc_disk_cmds_stats_done_reset(&_comp->lock_cmd);				\
} while (0)

#define this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_) ({			\
	struct nvmeibc_disk_percpu_cmds_stats *_pcp_ =						\
		this_cpu_ptr(_disk_->pcpu_cmds_stats);							\
	struct nvmeibc_disk_cmd_stats *_s_ = NULL;							\
	switch ((_cmd_)->cmd_type) {										\
	case NVMEIBC_DISK_CMD_IO:                                 		  	\
		_s_ = &_pcp_->io;												\
		break;                                                  		\
	case NVMEIBC_DISK_CMD_GEN:                                  		\
		_s_ = &_pcp_->gen;												\
		break;                                                  		\
	case NVMEIBC_DISK_CMD_LOCK:                                 		\
		_s_ = &_pcp_->lock;												\
		break;                                                  		\
	case NVMEIBC_DISK_CMD_LOCK_HW_OWNER:                           		\
		_s_ = &_pcp_->lock_hw_owner;									\
		break;                                                  		\
	case NVMEIBC_DISK_CMD_LOCK_HW_WBINFO:                          		\
		_s_ = &_pcp_->lock_hw_wbinfo;									\
		break;                                                  		\
	case NVMEIBC_DISK_CMD_LOCK_HW_READ:									\
		_s_ = &_pcp_->lock_hw_read;										\
		break;															\
	}																	\
	_s_;																\
})

#define nvmeibc_disk_cmds_stats_add(_s_, _name_, _cnt_)					\
do { (_s_)->_name_+=_cnt_; } while (0)

#define nvmeibc_disk_cmds_stats_inc(_s_, _name_)						\
do { (_s_)->_name_++; } while (0)

#define nvmeibc_disk_cmds_stats_dec(_s_, _name_)						\
do { (_s_)->_name_--; } while (0)

#define nvmeibc_disk_cmds_stats_done_reset(_cmd_)						\
do { (_cmd_)->stats_done.done = false; } while (0)

#if NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT
	#define NVMEIBC_DISK_CMDS_STATS_DBL_ADD_STACK \
	struct nvmeib_stack_trace st = {\
		.max_entries = ARRAY_SIZE((_cmd_)->st_ents), \
		.nr_entries = 0,\
		.entries = (_cmd_)->st_ents,\
		.skip = 0,\
	}\
	nvmeib_public_save_stack_trace(&st);
#else
	#define NVMEIBC_DISK_CMDS_STATS_DBL_ADD_STACK
#endif
#define nvmeibc_disk_cmds_stats_done_set_once(_cmd_, _type_, _comp_code_) 					\
do {																	\
	if (likely(!(_cmd_)->stats_done.done)) {	\
		NVMEIBC_DISK_CMDS_STATS_DBL_ADD_STACK;\
		(_cmd_)->stats_done.done = true;		\
		(_cmd_)->stats_done.type = _type_;	\
		(_cmd_)->stats_done.comp_code = _comp_code_;	\
		(_cmd_)->stats_done.stats_done_jif = jiffies;	\
		memcpy((_cmd_)->stats_done.task_name, current->comm, sizeof((_cmd_)->stats_done.task_name));	\
	} else {							\
		pr_err("module nvmeibc BUG: Double Completion: %px type=%d comp_code=%d by task %s. Crashing the system to prevent data corruption. Error code: 1059.\n", _cmd_, _type_, _comp_code_, (_cmd_)->stats_done.task_name);\
		BUG_ON(1);						\
	}									\
} while (0)

/* called before calling channel's execute-io so we'll inc
   the 'in-progress' stats before completion stats are inc
   by another ctx. On error, just do dec back */
#define nvmeibc_disk_cmds_stats_direct_exec_start(_disk_, _cmd_)		\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_submissions);				\
	nvmeibc_disk_cmds_stats_inc(_s_, n_direct_tot);						\
	nvmeibc_disk_cmds_stats_inc(_s_, n_executing);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_direct_exec_err(_disk_, _cmd_, _rv_)			\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_executing);						\
	nvmeibc_disk_cmds_stats_inc(_s_, n_err_direct_exec);				\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_completions);				\
	nvmeibc_disk_cmds_stats_done_set_once(_cmd_, STATS_DONE_DIRECT_EXEC_ERR, _rv_);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_pending_add(_disk_, _cmd_)				\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_submissions);				\
	nvmeibc_disk_cmds_stats_inc(_s_, n_pending_tot);					\
	nvmeibc_disk_cmds_stats_inc(_s_, n_pending_now);					\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_pending_timeout(_disk_, _cmd_)			\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_pending_now);					\
	nvmeibc_disk_cmds_stats_inc(_s_, n_err_pending_timeout);			\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_completions);				\
	nvmeibc_disk_cmds_stats_done_set_once(_cmd_, STATS_DONE_PENDING_TIMEOUT, -ETIMEDOUT);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_pending_aborted(_disk_, _cmd_)			\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_pending_now);					\
	nvmeibc_disk_cmds_stats_inc(_s_, n_err_pending_aborted);			\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_completions);				\
	nvmeibc_disk_cmds_stats_done_set_once(_cmd_, STATS_DONE_PENDING_ABORTED, -ENXIO);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_pending_exec_start(_disk_, _cmd_)		\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_pending_now);					\
	nvmeibc_disk_cmds_stats_inc(_s_, n_executing);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_pending_exec_err(_disk_, _cmd_)			\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_executing);						\
	nvmeibc_disk_cmds_stats_inc(_s_, n_err_pending_exec);				\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_completions);				\
	nvmeibc_disk_cmds_stats_done_set_once(_cmd_, STATS_DONE_PENDING_EXEC_ERR, -EIO);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_err_internal_retry(_disk_, _cmd_)		\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_executing);						\
	nvmeibc_disk_cmds_stats_inc(_s_, n_err_internal_retry);				\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_llp_complete(_disk_, _cmd_, _type_, _comp_code_)				\
do {																	\
	struct nvmeibc_disk_cmd_stats *_s_ =								\
		this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);				\
	if (_s_) {															\
	nvmeibc_disk_cmds_stats_dec(_s_, n_executing);						\
	nvmeibc_disk_cmds_stats_inc(_s_, n_llp_completions);				\
	nvmeibc_disk_cmds_stats_inc(_s_, n_ulp_completions);				\
	nvmeibc_disk_cmds_stats_done_set_once(_cmd_, _type_, _comp_code_);						\
	}																	\
} while (0)

#define nvmeibc_disk_cmds_stats_record_poison_check_hit(_disk_, _cmd_, _cnt_)  \
	do {                                                                       \
		struct nvmeibc_disk_cmd_stats *_s_ =                                   \
		    this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);                \
		if (_s_) {                                                             \
			nvmeibc_disk_cmds_stats_add(_s_, n_llp_poison_check_hits, _cnt_);  \
		}                                                                      \
	} while (0)

#define nvmeibc_disk_cmds_stats_record_posion_check_miss(_disk_, _cmd_, _cnt_) \
	do {                                                                       \
		struct nvmeibc_disk_cmd_stats *_s_ =                                   \
		    this_cpu_disk_cmd_stats_ptr_by_type(_disk_, _cmd_);                \
		if (_s_) {                                                             \
			nvmeibc_disk_cmds_stats_add(_s_, n_llp_poison_check_misses, _cnt_);\
		}                                                                      \
	} while (0)

#else /* NVMEIBC_DISK_CMDS_STATS !defined */

#define nvmeibc_disk_cmds_stats_init_lock_cmd(_x_, _comp)
#define nvmeibc_disk_cmds_stats_done_reset(_cmd_)
#define nvmeibc_disk_cmds_stats_direct_exec_start(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_direct_exec_err(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_pending_add(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_pending_timeout(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_pending_aborted(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_pending_exec_start(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_pending_exec_err(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_internal_retry(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_err_internal_retry(_disk_, _cmd_)
#define nvmeibc_disk_cmds_stats_llp_complete(_disk_, _cmd_, _type_, _comp_code_)
#define nvmeibc_disk_cmds_stats_record_poison_check_hit(_disk_, _cmd_, _cnt_)
#define nvmeibc_disk_cmds_stats_record_posion_check_miss(_disk_, _cmd_, _cnt_)
#endif

void *nvmeibc_disk_create_globals(const struct nvmeibc_cinst_params_core *p);
void  nvmeibc_disk_delete_globals(const struct nvmeibc_cinst_params_core *p);

void nvmeibc_disk_complete_gen_cmd(union nvmeib_gen_cmd_rsp *rsp, int rv);

/* PBLR - PiggyBack Lock Read */
#define NVMEIBC_DISK_CMD_PBLR_POISON_CH_MASK (~((1 << NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT) -1))
#define NVMEIBC_DISK_CMD_PBLR_POISON_LOCAL	((NVMEIBC_DISK_CMD_PBLR_POISON_MSBS << NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT)| 0x11)
#define NVMEIBC_DISK_CMD_PBLR_POISON_RDDA	((NVMEIBC_DISK_CMD_PBLR_POISON_MSBS << NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT)| 0x22)
#define NVMEIBC_DISK_CMD_PBLR_POISON_NORDDA	((NVMEIBC_DISK_CMD_PBLR_POISON_MSBS << NVMEIBC_DISK_CMD_PBLR_POISON_CH_SHIFT)| 0x33)
void nvmeibc_disk_cmd_piggyback_lock_read_poison_inject(
	struct nvmeibc_disk_io_command *bcmd, u64 poison);
void nvmeibc_disk_cmd_piggyback_lock_read_poison_verify(
	struct nvmeibc_disk_io_command *bcmd);

#define nvmeibc_disk_cmd_piggyback_lock_read_poison_is_val_posioned(__val) \
	(((__val) & NVMEIBC_DISK_CMD_PBLR_POISON_CH_MASK) == NVMEIBC_DISK_CMD_PBLR_POISON_MSBS)

int local_nic_prio_cmp_fn(void *priv, struct list_head *a, struct list_head *b);

void nvmeibc_disk_inc_io_chan(struct nvmeibc_disk* disk);
void nvmeibc_disk_dec_io_chan(struct nvmeibc_disk* disk);

#define nvmeibc_disk_is_bcmd_jour_write(_req) (_req)->jam_op.n_ops

#define nvmeibc_set_lock_read(_bc, _dc, _all, _id, _bi) do { \
	if ((_bc)->comp.comp_code == 0) { \
		if (!(_dc)->lock_cnsts->w_blkset_info) \
			(_dc)->lock.id = _all; \
		else { \
			(_dc)->lock.id = _id; \
			(_dc)->lock.bi = _bi; \
		} \
	} \
} while (0)

extern int nvmeibc_disk_prefix_priority_masks_len;
extern unsigned int nvmeibc_disk_prefix_priority_masks[16];
int nvmeibc_disk_prefix_priority_masks_validate_module_params(void);

/* Disk coremask macros */
#define NVMEIBC_DISK_SHOULD_DEFER_TO_PCPU_WQ(_cond, _cpus) \
       ((_cond) && !NVMEIB_CPU_MASK_IS_EMPTY(_cpus))
#define NVMEIBC_DISK_GET_RESCHED_CPU(_cpus, _pcpu_cntr) \
       find_nth_bit(_cpus, NVMEIB_CPU_MASK_MAX_CPUS, (_pcpu_cntr++) % bitmap_weight(_cpus, NVMEIB_CPU_MASK_MAX_CPUS))
#define NVMEIBC_DISK_SAFE_TEST_CURRENT_CPU_IN_BITMAP(_cpus_ptr) \
       ({ int __cpu = smp_processor_id(); (__cpu < NVMEIB_CPU_MASK_MAX_CPUS && test_bit(__cpu, (_cpus_ptr)->cpus)); })

/* Disk coremask stats */
struct nvmeibc_disk_coremask_pcpu_stats {
	/* Coremask IO Stats */
	u64 n_io_not_coremask_op;
	u64 n_io_coremask_op;
	u64 n_io_coremask_submit_cpu_not_in_mask;
	u64 n_io_coremask_not_exist;
	u64 n_io_coremask_dying;
	u64 n_io_coremask_uid_mismatch;
	u64 n_io_coremask_nrch;
	u64 n_io_coremask_no_nrch;
	u64 n_io_coremask_nrch_busy;
	u64 n_io_coremask_pending_push;
	u64 n_io_coremask_pending_pop;
	u64 max_io_coremask_pending;
	/* IO reuse stats */
	u64 n_reuse_io_coremask_dying;
	u64 n_reuse_io_coremask_uid_mismatch;
	u64 n_reuse_io_coremask_submit_cpu_not_in_mask;
	u64 n_reuse_io_coremask_op;
	/* Coremask Lock Stats */
	u64 n_lock_not_coremask_op;
	u64 n_lock_coremask_op;
	u64 n_lock_coremask_not_exist;
	u64 n_lock_coremask_no_lock_ch;
	u64 n_lock_coremask_submit_cpu_not_in_mask;
	u64 n_lock_coremask_uid_mismatch;
};

#define COREMASK_PCPU_STAT_INC(_cinfo, _stat)		do {\
	(_cinfo)->_stat++;\
} while(0)

#define COREMASK_PCPU_STAT_DEC(_cinfo, _stat)		do {\
	(_cinfo)->_stat--;\
} while(0)
	
#define COREMASK_PCPU_STAT_ADD(_cinfo, _stat, _add)	do {\
	(_cinfo)->_stat += _add;\
} while(0)

#define COREMASK_PCPU_STAT_MAX(_cinfo, _stat, _max)	do {\
	(_cinfo)->_stat = max_t(typeof(_max), (_cinfo)->_stat, _max);\
} while(0)

struct nvmeibc_disk_coremask_pcpu_stats __percpu *nvmeibc_disk_get_coremask_stats_this_cpu(struct nvmeibc_disk *disk);
int nvmeibc_disk_notify_coremask_update(struct nvmeibc_idisk *disk);

#endif
