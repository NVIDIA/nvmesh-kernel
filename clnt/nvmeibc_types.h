/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TYPES_H
#define NVMEIBC_TYPES_H

#include "kr_incs.h"
#include "ib_incs.h"
#include "nvmeib.h"
#include "nvmeib_types.h"
#include "nvmeibs_types.h"
#include "nvmeib_shared.h"
#include "nvmeib_rdma.h"
#include "nvmeib_trend.h"
#include "nvmeib_measured_work.h"

enum {
	/* max rdma reads to get the disk completion phase bit right */
	NVMEIBC_MAX_IB_OVEREAGER = 1000000,
	/* the size of a volume device shared receive queue */
#ifndef LOW_MEM
	NVMEIBC_MAX_VOLUME_SRQ = 1024,
#else
	NVMEIBC_MAX_VOLUME_SRQ = 128,
#endif
	/* the maximum size of an S/G list the block device may issue.
	   in general it should be big enough to hold a full S/G that
	   we could not collapsed but due to the fact that the buffers are
	   always n * 4K we should aways be able to collapse.
	   There is another issue that making the
	   io channel qp s/g  size larger that 1 screws the performance
	   so we set s/g size to be 1
	*/
	NVMEIBC_MAX_IO_CHANNEL_SG = NVMEIB_DEF_SG_PER_WQE,

	/* Size of Admin RQ - If SRQ is not being used*/
	NVMEIBC_ADMIN_RQ_SIZE = 16,
	/* the max number of completion that can extracted from a cq poll */
	NVMEIBC_POLL_SIZE = (NVMEIBS_MAX_IO_CHANNEL_MSGS >> 1),
	/* the minimum number of disk resources that beyond  it
	   block commands may may be issue on the nordda channel
	*/
	NVMEIBC_WATERMARK_GOTO_NORDDA = 256,
	/* net retry number */
	NVMEIBC_DEF_RETRY_COUNT = 7,
	/* how many seconds to wait between disk rediscover calls */
	NVMEIBC_REDISCOVER_WAIT = (30 * HZ),
	/* seconds to wait for block device to pause */
	NVMEIB_WAIT_BLOCK_DEV_PAUSE = (10 * HZ),
	/* for local disks, we can wait essentially forever because,
	 * there is a guarantee that the nvme layer will eventually finish */
	NVMEIB_WAIT_BLOCK_DEV_PAUSE_LOCAL = INT_MAX,

	/* general definition for network timeout */
	NVMEIBC_IO_TIMEOUT = 1,

	/* minimun WD timeout for any net/qp  */
	NVMEIBC_MIN_IO_TIMEOUT = 3 * NVMEIBC_IO_TIMEOUT,

	/* no reply from remote timeout */
	NVMEIBC_IO_NO_REPLY_TIMEOUT = 2 * NVMEIBC_IO_TIMEOUT,

	/* the longest time we wait for IO to finish */
	NVMEIBC_IO_LONG_TIMEOUT = (10 * HZ),
};

/* a disk that can we accessed remotely */
#define DISK_DISCOVER_STATUS_FORCE(_disk, _status) 					\
do { 											\
	nvmeib_trend_update_head(&(_disk->discover_trend), _status); \
} while (0)

#define DISK_DISCOVER_STATUS(_disk, _status) 					\
do { 											\
	if ((NVMEIB_TREND_HEAD(_disk->discover_trend).data) == 0) {		\
		DISK_DISCOVER_STATUS_FORCE(_disk, _status); \
	}											\
} while (0)

#define ARNIC_DISCOVER_STATUS(_arnic, _status) 					\
do { 											\
	if ((NVMEIB_TREND_HEAD(_arnic->discover_trend).data) == 0) {		\
		nvmeib_trend_update_head(&(_arnic->discover_trend), _status); \
	}											\
} while (0)

/**
 * holds the number of admin connection to a nic.
 */
struct nvmeibc_arnic_score {
	struct list_head link;
	atomic_t n_connections;
	union ib_gid gid;
};

/* NIC priority breakdown */
union nic_priority {
	struct {
		/* First draft, need to check how the order of these effects performance */
#ifdef __LITTLE_ENDIAN
		u8 numa_dist; 	/* NVMEIBS_DISK_NIC_NUMA_SAME = 10, NVMEIBS_DISK_NIC_NUMA_DIFF = 20, */
		u8 transport; 	/* NVMEIB_IB_PORT_PRIORITY = 0, NVMEIB_ROCE_PORT_PRIORITY = 10, NVMEIB_TCP_PORT_PRIORITY = 20, */
		u8 latency;		/* Not implemented yet, always 0 */
		u8 bw;			/* Not implemented yet, always 0 */
#else
		u8 bw;
		u8 latency;
		u8 transport;
		u8 numa_dist;
#endif
	};
	u32 raw;
};

/* remote nic that uses to admin a controller machine - it is assumed that
   an admin nic listens for client connect requests
*/
struct nvmeibc_admin_rnic {
	/* the nic id */
	union ib_gid hw_gid;
	union ib_gid ib_gid;
	bool is_multi_transport;
	__be16 pkey;
	__be64 service_id;
	__be16 service_port;
	enum rdma_link_layer link_layer;
	enum rdma_transport_type transport_type;
	/* true if nic is local to client machine */
	bool local;
	struct nvmeibc_ib_port *local_port; /* Pointer to port if local */
	/* true if remote admin nic is real, namely there is controller to
	   support it
	*/
	bool alive;
	/* True if a (user) rule on the server side prefers this nic */
	bool prefered;
	/* Priority of the nic on the server side, based on numa distance, protocol, bw and latency
	   0 - numas are not connected, 10 - same numa (chip), 20, ... */
	union nic_priority priority;
	/* Number of connection on the target port when the client read io rsc */
	int num_conns;
	/* Order of the nic in the origin arnics list */
	int order;
	/* the client admin channel to pair with this nic */
	struct nvmeibc_admin_channel *channel;
	struct list_head link;
	struct nvmeibc_arnic_score *score;
	char node_id[NVMEIB_HOST_NAME_LEN];
	struct nvmeib_trend discover_trend;
};

struct nvmeibc_io_path {
	int idx;
	int n_chs;
	bool prefered;
	struct list_head iopaths_link;
	uint32_t priority;
};

/**
 * struct nvmeibc_io_lnic: a client nic that is capable to
 * interact with disks on controller machines
 */
struct nvmeibc_io_rnic;
struct nvmeibc_io_lnic {
	union {
		struct nvmeibc_ib_port *port;
	};

	/* cached gid attrs */
	bool may_access;
	union ib_gid ib_gid;
	enum rdma_link_layer layer;
	enum rdma_transport_type transport_type;

	union {
		struct ib_sa_path_rec path;
	};
	enum nvmeib_rdma_type rdma_type;
	bool path_valid;
	spinlock_t spinlock;
	/* 1 if we are in the process of disappearing */
	atomic_t dying;
	/* the peer remote nic */
	struct nvmeibc_io_rnic *rionic;
	/* number of qp we maintain for nordda */
	int n_nr_qps;
	/* the access channels (channel ~ net ~ qp) that use the nic for nordda */
	struct nvmeibc_ib_nordda_channel *nr_channels;
	/* the remote disk that io_channels from this nic can access */
	struct nvmeibc_disk *disk;
	/* link in rionic's (rdda) lionics list */
	struct list_head rionic_link;
	/* link in rionic's nordda lionics list */
	struct list_head rionic_nrlink;

	u32 start_ioch_attempt_id;		/* is first attempt, in the current start-io-chs round/work, for this lionic (=path) */
	int start_ioch_attempt_ctr;		/* num of  attempts, in the current start-io-chs round/work, to connect this lionic (=path) */
	int start_ioch_path_fail_ctr; 	/* num of  failures, in the current start-io-chs round/work, to connect this lionic (=path) due to path-error (see start_ioch_path_error())  */

	u64 __percpu *last_io_ka_jif;
	u64 __percpu *last_send_success_jif;
	u64 __percpu *last_recv_success_jif;
	int last_ka_ch_idx;

	/* link to iopaths (on-stack) list used by start-io-channels */
	struct nvmeibc_io_path iopath;

	/* dummy MD mapped to local NIC */
	void *dummy_md_read_ptr;
	dma_addr_t dummy_md_read_addr;
	void *dummy_md_write_ptr;
	dma_addr_t dummy_md_write_addr;
};

#define iop_to_lionic(_iop) container_of(_iop, struct nvmeibc_io_lnic, iopath)

/**
 * struct nvmeibc_io_rnic: a controller machine nic that is
 * capable to pair with local io nic
 */
struct nvmeibc_channel;
struct nvmeibc_io_rnic {
	/* the remote io nic id */
	union ib_gid hw_gid;
	/* the srv allow access to this rnic */
	u8 may_access;
	/* is a local rionic */
	bool local;
	union {
		union ib_gid ib_gid;
	};
	union {
		u16 pkey;
	};
	union {
		u16 hw_type;
	};
	enum rdma_link_layer layer;
	enum rdma_transport_type transport_type;
	union nic_priority priority;
	/* Uniquely identifies the NIC (as opposed to the hw_gid which uniquely identifies a port) */
	__be64 node_guid;
	/* TCP Base port */
	unsigned tcp_base_port;
	/* TCP Num ports */
	unsigned tcp_num_ports;
	/* when we read data from disk an S/G list on the client side turns
	   into a bunch of separate messages on the controller side.
	   the max number of such messages is saved here
	*/
	u16 n_msgs;
	/* 1 if we are in the process of disappearing */
	atomic_t dying;
	/* the admin channel we belong to */
	struct nvmeibc_admin_channel *ch;
	/* the local io nics that can pair with this remote io nic independent
	   of which disk
	*/
	struct list_head lionics;
	struct list_head nr_lionics;

	/* link inside the admin channel */
	struct list_head admin_link;
	/* link in disk's (rdda) rionics list */
	struct list_head disk_link;
	/* link in disk's nordda rionics list */
	struct list_head disk_nrlink;
	/* no-rdda prefered (by user or srv/numa-distance) for io */
	bool nr_prefered;
	/* if rionic is not accessible from any of its lionics,
	   choose sibling non-prefered rionic to substitute */
	struct nvmeibc_io_rnic *sub;
	/* well one always needs a lock... */
	spinlock_t spinlock;
	/* back pointer to the disk */
	struct nvmeibc_disk *disk;
};

/* There are D disks and V volumes with relation of many-to-many. The struct
 * below normalizes this relashion by represents a disk per volume.
 * at most DxV such structs exist in the system */
struct nvmeibc_disk;
struct nvmeibc_volume;
struct nvmeibc_block_device;
struct nvmeibc_disk_id {
	/* the disk id */
	char name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	//the attachment version of the attach message that created the disk
	int attachment_version;
	/* the volume the disk_id belongs to. Volume has many disk_id's */
	struct nvmeibc_volume *volume;
	/* the link in the volume struct */
	struct list_head link;
	/* the link into a list of disk_id's of struct nvmeibc_disk*/
	struct list_head slink;
	/* reference to the physical disk which may participate in many volumes */
	struct nvmeibc_disk *disk;
	/* Number of active ranges (talk to TOMA) that volume uses on that disk */
	int num_ranges;
	/* The list of disk_ids on a target */
	struct list_head dlink;
	/* The target which holds all of the nics available for it's node */
	struct nvmeibc_target *target;
	/* mark disk as new */
	int new_disk;
	/* Disk id creation time in jiffies*/
	ulong create_jiff;
	/* Disk statistics for this volume, Read/Write/Trim */
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	struct nvmeib_io_stats *v_disk_stats;
#endif
	/* Proc folder and files to print above stats (json) on demand */
	struct proc_dir_entry *proc_dir;
	struct nvmeib_public_procfs_ent *proc_ent_stats;
	struct nvmeib_public_procfs_ent *proc_ent_stats_json;
	/* ----- Attention: Temp variables, used during reconfiguration, to speed
	   it, Store garbage value when configuration handling is over ---*/
	struct list_head disk_wl_link; /* List of deffered disk_ids*/
};

/* Used to update disk params according to the volume represented by block_dev*/
struct nvmeibc_disk_id_update_params{
	struct nvmeibc_idisk *disk;
	struct nvmeibc_block_device *block_dev; /* Block device of volume*/
	bool is_attach;
};

/**
 * nvmeibc_volume_info: filled by the management.  it contains
 * two lists.  the arnics is a list of struct nvmeibc_admin_rnic
 * and the disks is a list of struct nvmeibc_disk.
 * MODIFIED: arnics list is no longer used on the volume itself, rather
 * it is held by the target that holds the disk, the disk_id points to the
 * target to access the list of it's nics
 */
struct nvmeibc_volume_info {
	//struct list_head arnics; // No longer used
	struct list_head disks; /* list of struct nvmeibc_disk_id */
	bool retain_disks; // when draining IO during detaching, do not release disks one by one with each destroyed segment, but rather do it in parallel in the detach SM after destroying topologies
};

#ifdef DEBUG_TRANSFERS
#define DEBUG_TRANSFERS_STACK_SIZE 8
struct nvmeibc_transfer_reason {
	int type;					// RDMAS (0..100), IO data (1000+)
	struct list_head link;
	int stack[DEBUG_TRANSFERS_STACK_SIZE];		// Stack of last actions
	int sp;										// Stack pointer (cyclic)
	int inc_cpu;
};
#endif // DEBUG_TRANSFERS

enum nvmeibc_disk_command_type {
	NVMEIBC_DISK_CMD_IO 	= 0x10,				// R/W/T blocks to disk
	NVMEIBC_DISK_CMD_GEN	= 0x11,				// GEN commands
	NVMEIBC_DISK_CMD_LOCK	= 0x12,				// Lock SW commands

	/* currently used only for stats but plan is
	   to merge all Transport's ops to same enum */
#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)
	NVMEIBC_DISK_CMD_LOCK_HW_OWNER	= 0x13,		// Owner-Lock HW commands
  //NVMEIBC_DISK_CMD_LOCK_HW_ACTIVE	= 0x14,		// Active-Lock HW commands
	NVMEIBC_DISK_CMD_LOCK_HW_WBINFO	= 0x15,		// Write-Binfo-Lock HW commands
	NVMEIBC_DISK_CMD_LOCK_HW_READ	= 0x16,		// Rad-Lock HW commands
#endif
};

enum nvmeibc_disk_cmd_status {
	NVMEIBC_DISK_CMD_NOT_INIT				= 0x00,
	NVMEIBC_DISK_CMD_INIT 					= 0x01,
	NVMEIBC_DISK_CMD_DISK_DYING				= 0xfe,
	NVMEIBC_DISK_CMD_COMPLETED 				= 0xff,

	/* Local */
	NVMEIBC_DISK_CMD_LOCAL					= 0x10,
	NVMEIBC_DISK_CMD_LOCAL_MD_TRIM_RD_COMP	= 0x11,
	NVMEIBC_DISK_CMD_LOCAL_COMPLETED		= 0x12,

	/* Remote */
	NVMEIBC_DISK_CMD_REMOTE				= 0x20,
	NVMEIBC_DISK_CMD_REMOTE_GOT_CH			= 0x21,
	NVMEIBC_DISK_CMD_REMOTE_ADD_PENDING	= 0x22,
	NVMEIBC_DISK_CMD_REMOTE_PENDING_FAILED	= 0x23,
	NVMEIBC_DISK_CMD_REMOTE_PENDING_DROP	= 0x24,
	NVMEIBC_DISK_CMD_REMOTE_COMPLETED		= 0x25,

	/* No-RDDA */
	NVMEIBC_DISK_CMD_NORDDA_EXECUTE 		= 0x30,
	NVMEIBC_DISK_CMD_NORDDA_SENT			= 0x31,
	NVMEIBC_DISK_CMD_NORDDA_FAILED_SEND		= 0x32,

	/* RDDA */
	NVMEIBC_DISK_CMD_RDDA_EXECUTE			= 0x40,
	NVMEIBC_DISK_CMD_RDDA_RECV_COMP		= 0x41,
	NVMEIBC_DISK_CMD_RDDA_RECV_COMP_OE		= 0x42,
	NVMEIBC_DISK_CMD_RDDA_OE_CONT_WAIT		= 0x43,
	NVMEIBC_DISK_CMD_RDDA_OE_FINISH_READ	= 0x44,
};

static inline const char *nvmeibc_disk_cmd_status_to_str(
	enum nvmeibc_disk_cmd_status sts)
{
	switch (sts) {
	case NVMEIBC_DISK_CMD_NOT_INIT				: return "NOT_INIT";
	case NVMEIBC_DISK_CMD_INIT                  : return "INIT";
	case NVMEIBC_DISK_CMD_LOCAL                 : return "LOCAL";
	case NVMEIBC_DISK_CMD_LOCAL_MD_TRIM_RD_COMP : return "LOCAL_MD_TRIM_RD_COMP";
	case NVMEIBC_DISK_CMD_LOCAL_COMPLETED       : return "LOCAL_COMPLETED";
	case NVMEIBC_DISK_CMD_REMOTE                : return "REMOTE";
	case NVMEIBC_DISK_CMD_REMOTE_GOT_CH         : return "REMOTE_GOT_CH";
	case NVMEIBC_DISK_CMD_REMOTE_ADD_PENDING    : return "REMOTE_ADD_PENDING";
	case NVMEIBC_DISK_CMD_REMOTE_PENDING_FAILED : return "REMOTE_PENDING_FAILED";
	case NVMEIBC_DISK_CMD_REMOTE_PENDING_DROP   : return "REMOTE_PENDING_DROP";
	case NVMEIBC_DISK_CMD_COMPLETED             : return "COMPLETED";
	case NVMEIBC_DISK_CMD_NORDDA_EXECUTE        : return "NORDDA_EXECUTE";
	case NVMEIBC_DISK_CMD_NORDDA_SENT           : return "NORDDA_SENT";
	case NVMEIBC_DISK_CMD_RDDA_EXECUTE          : return "RDDA_EXECUTE";
	case NVMEIBC_DISK_CMD_RDDA_RECV_COMP        : return "RDDA_RECV_COMP";
	case NVMEIBC_DISK_CMD_RDDA_RECV_COMP_OE     : return "RDDA_RECV_COMP_OE";
	case NVMEIBC_DISK_CMD_RDDA_OE_CONT_WAIT     : return "RDDA_OE_CONT_WAIT";
	case NVMEIBC_DISK_CMD_RDDA_OE_FINISH_READ   : return "RDDA_OE_FINISH_READ";
	default: return "???";
	}
}

enum nvmeibc_disk_discovery_stages {
	DD_STG_DISCOVER_MAIN,
	DD_STG_FILL_LOCAL_NICS,
	DD_STG_MARK_LOCAL_NICS,
	DD_STG_CREATE_ADMIN_CHANNELS,
	DD_STG_ALLOC_DIRTY_BITS_MEM,
	DD_STG_MAP_DBITS_TO_NICS,
	DD_STG_SET_ARNIC_PORTS_WITH_LNIC_SRC_PORT,
	DD_STG_CHECK_ARNICS_ACCESS,
	DD_STG_READ_ALL_IO_RSCS,
	DD_STG_CREATE_WQ,
	DD_STG_LOCAL_DISK_CL_REGISTER,
	DD_STG_START_LOCAL_LOCK_CHANNEL,
	DD_STG_GET_LOCAL_JRNL_RNG,
	DD_STG_NVMEIBC_DISK_TOMA_CREATE_LOCAL,
	DD_STG_ACCESS_RIONICS_USING_LPORTS,
	DD_STG_CHECK_DISKS_ACCESS,
	DD_STG_CREATE_ACCESS_MAP,
	DD_STG_GET_JOURNAL_RANGE,
	DD_STG_CHECK_TRANSPORT_TYPE,
	DD_STG_REQUEST_DISKS_RESOURCES,
	DD_STG_NVMEIBC_DISK_TOMA_CREATE_REMOTE,
	DD_STG_NVMEIBC_DISK_START_PERIODIC_IOCH_STARTER,
};

static inline const char *nvmeibc_disk_discovery_stages_to_str(enum nvmeibc_disk_discovery_stages stage){
	switch (stage)
	{
	case DD_STG_DISCOVER_MAIN								:return "STAGE_DISCOVER_MAIN";
	case DD_STG_FILL_LOCAL_NICS							:return "STAGE_FILL_LOCAL_NICS";
	case DD_STG_MARK_LOCAL_NICS							:return "STAGE_MARK_LOCAL_NICS";
	case DD_STG_CREATE_ADMIN_CHANNELS						:return "STAGE_CREATE_ADMIN_CHANNELS";
	case DD_STG_ALLOC_DIRTY_BITS_MEM						:return "STAGE_ALLOC_DIRTY_BITS_MEM";
	case DD_STG_MAP_DBITS_TO_NICS							:return "STAGE_MAP_DBITS_TO_NIC";
	case DD_STG_SET_ARNIC_PORTS_WITH_LNIC_SRC_PORT			:return "STAGE_SET_ARNIC_PORTS_WITH_LNIC_SRC_PORT";
	case DD_STG_CHECK_ARNICS_ACCESS						:return "STAGE_CHECK_ARNICS_ACCESS";
	case DD_STG_READ_ALL_IO_RSCS							:return "STAGE_READ_ALL_IO_RSCS";
	case DD_STG_CREATE_WQ									:return "CREATE_WQ";
	case DD_STG_LOCAL_DISK_CL_REGISTER						:return "STAGE_LOCAL_DISK_CL_REGISTER";
	case DD_STG_START_LOCAL_LOCK_CHANNEL					:return "STAGE_START_LOCAL_LOCK_CHANNEL";
	case DD_STG_GET_LOCAL_JRNL_RNG							:return "STAGE_GET_LOCAL_JRNL_RNG";
	case DD_STG_NVMEIBC_DISK_TOMA_CREATE_LOCAL				:return "STAGE_NVMEIBC_DISK_TOMA_CREATE_LOCAL";
	case DD_STG_ACCESS_RIONICS_USING_LPORTS				:return "STAGE_ACCESS_RIONICS_USING_LPORTS";
	case DD_STG_CHECK_DISKS_ACCESS							:return "STAGE_CHECK_DISKS_ACCESS";
	case DD_STG_CREATE_ACCESS_MAP							:return "STAGE_CREATE_ACCESS_MAP";
	case DD_STG_GET_JOURNAL_RANGE							:return "STAGE_GET_JOURNAL_RANGE";
	case DD_STG_CHECK_TRANSPORT_TYPE						:return "STAGE_DISK_IS_TRANSPORT_TCP";
	case DD_STG_REQUEST_DISKS_RESOURCES					:return "STAGE_REQUEST_DISKS_RESOURCES";
	case DD_STG_NVMEIBC_DISK_TOMA_CREATE_REMOTE			:return "STAGE_NVMEIBC_DISK_TOMA_CREATE_REMOTE";
	case DD_STG_NVMEIBC_DISK_START_PERIODIC_IOCH_STARTER	:return "STAGE_NVMEIBC_DISK_START_PERIODIC_IOCH_STARTER";
	default:	return "???";
	}
}

enum stats_done_info_type {
	STATS_DONE_TYPE_NONE,
	STATS_DONE_DIRECT_EXEC_ERR,
	STATS_DONE_PENDING_TIMEOUT,
	STATS_DONE_PENDING_ABORTED,
	STATS_DONE_PENDING_EXEC_ERR,
	STATS_DONE_LLP_COMPLETE_IOCH_DRAINED,
	STATS_DONE_LLP_COMPLETE_IO_RESPONSE,
	STATS_DONE_LLP_COMPLETE_IO_RESPONSE_BUF_SAVE,
	STATS_DONE_LLP_COMPLETE_IO_RESPONSE_BUF_REUSE,
	STATS_DONE_LLP_COMPLETE_IO_RESPONSE_BUF_REUSE_DEL,
	STATS_DONE_LLP_COMPLETE_IO_RESPONSE_FINALIZE,
	STATS_DONE_LLP_COMPLETE_LLP_LOCAL_IO_CB,
	STATS_DONE_LLP_COMPLETE_LLP_LOCAL_LOCK_WORK,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_GEN_CMD,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_GEN_CMD_NO_RDDA,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_IO_CHANNEL,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_LOCK_CMD_NO_RDDA,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_OE_FINISH_WRITE_BUF_SAVE,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_OE_FINISH_WRITE_BUF_REUSE,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_OE_FINISH_WRITE_BUF_REUSE_DEL,
	STATS_DONE_LLP_COMPLETE_LLP_COMPLETE_OE_FINISH_READ,
};

struct stats_done_info {
	bool done; /* the rest only valid if done is true */
	enum stats_done_info_type type;
	int comp_code;
	u64 stats_done_jif;
	char task_name[PROC_NAME_LEN];
};


#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)
/* see BUILD_BUG_ON that @cmd_type has same offset as in nvmeibc_disk_command */
struct nvmeibc_disk_command_stats_only {
	enum nvmeibc_disk_command_type cmd_type;
	struct stats_done_info stats_done;
#if defined(NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT) && (NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT==1)
	unsigned long st_ents[5];
#endif
};
#endif

struct nvmeibc_dev;
struct nvmeibc_disk_command_workqe {
	struct workqe_struct work;
	struct nvmeibc_dev *dev;
	enum stats_done_info_type done_type;
};

struct nvmeibc_disk_command {
	enum nvmeibc_disk_command_type cmd_type;
	bool server_side_only;
	bool complete_w_error;
	struct list_head dcmd_link;            // Don't touch, used by low level layer: link for command listing.
	void *owner;
	union {
		struct work_struct auto_fail_no_rdda_work;	// Transp layer: Autofail in no_rdda_channel
		struct work_struct auto_fail_work;			// Block  layer: Auto-fail command (without sending to server), do this asyncronously using this work-item
		struct measured_work view_lock_work;		// Block  layer: Retry view lock work
		ktime_t start_ts;								// c_disk layer: request start time stamp. Used to measure latency
	};
	int cb_comp_code;								// Todo: remove me
	unsigned long post_jif;
	/**************** DEBUG ********************/
#ifdef DEBUG_TRANSFERS
	struct nvmeibc_transfer_reason reason;	// Who issued this disk transfer, used to detect which transfer hasn't finished and prevents disk PAUSE
	bool in_flight;
#endif

#if defined(NVMEIBC_DISK_CMDS_STATS) && (NVMEIBC_DISK_CMDS_STATS==1)
	struct stats_done_info stats_done;
#if defined(NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT) && (NVMEIBC_DISK_CMDS_STATS_DBL_COMP_BT==1)
	unsigned long st_ents[5];
#endif
#endif

#ifdef NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED
	char pre_poison[64];
	struct nvmeibc_disk *disk;
	struct nvmeibc_channel *ch;
	int init_cpu;
	void *ch_ctx;
	struct list_head uncompleted_list_n;    // Linked List of uncompleted commands in the nvmeibc_disk.
	enum nvmeibc_disk_cmd_status cmd_status, prev_cmd_status;
	const char *file_status, *prev_file_status;
	int line_status, prev_line_status;
	char post_poison[64];
#endif

	bool defer_cb;							/* defer running cb helper */
	void (*ulp_cb)(struct nvmeibc_disk_command *, enum stats_done_info_type, struct nvmeibc_dev *);
	enum stats_done_info_type ulp_cb_done_type;
	const struct nvmeib_cpu_mask_info *cpu_mask_info;
	struct nvmeibc_disk_command_workqe work; /* for deferring on pcpu wq */
	bool local_cmd;							/* bool, is the command local or remote */
};

#ifdef NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED

#define nvmeibc_disk_cmd_status_debug(_disk_cmd_, _status_)\
do {\
	if ((_disk_cmd_)->cmd_status == NVMEIBC_DISK_CMD_COMPLETED && (_status_) != NVMEIBC_DISK_CMD_INIT) \
		BUG_ON(_status_ != NVMEIBC_DISK_CMD_COMPLETED); /* If command is already completed, then additional completes are okay, but we do nothing */\
	else if (((_disk_cmd_)->cmd_status == NVMEIBC_DISK_CMD_NOT_INIT && (_status_) == NVMEIBC_DISK_CMD_COMPLETED)) {\
		/* Do nothing, we are going straight from not-init to completed */ \
	} else {\
		BUG_ON((_disk_cmd_)->cmd_status == NVMEIBC_DISK_CMD_NOT_INIT && (_status_) != NVMEIBC_DISK_CMD_INIT); /* Not Init, can only go to Init */\
		BUG_ON((_disk_cmd_)->cmd_status == (_status_));\
		(_disk_cmd_)->prev_cmd_status = ((_disk_cmd_)->cmd_status);\
		(_disk_cmd_)->prev_file_status = ((_disk_cmd_)->file_status);\
		(_disk_cmd_)->prev_line_status = ((_disk_cmd_)->line_status);\
		(_disk_cmd_)->cmd_status = (_status_);\
		(_disk_cmd_)->file_status = __FILE__;\
		(_disk_cmd_)->line_status = __LINE__;\
		if ((_status_) == NVMEIBC_DISK_CMD_INIT || (_status_) == NVMEIBC_DISK_CMD_COMPLETED) {\
			unsigned long flags; \
			spin_lock_irqsave(&(_disk_cmd_)->disk->uncompleted_spinlock[(_disk_cmd_)->init_cpu], flags); \
			if ((_status_) == NVMEIBC_DISK_CMD_INIT) {\
				list_add_tail(&(_disk_cmd_)->uncompleted_list_n, &(_disk_cmd_)->disk->uncompleted_cmds[(_disk_cmd_)->init_cpu]); \
				(_disk_cmd_)->disk->n_uncompleted_cmds[(_disk_cmd_)->init_cpu]++;\
			} else if ((_status_) == NVMEIBC_DISK_CMD_COMPLETED){\
				list_del_init(&(_disk_cmd_)->uncompleted_list_n); \
				(_disk_cmd_)->disk->n_uncompleted_cmds[(_disk_cmd_)->init_cpu]--;\
			} else \
				BUG_ON(1);\
			spin_unlock_irqrestore(&(_disk_cmd_)->disk->uncompleted_spinlock[(_disk_cmd_)->init_cpu], flags);\
		}\
	}\
} while (0)

#define nvmeibc_disk_cmd_status_debug_got_remote_ch(_disk_cmd_, _ch_, _ch_ctx_)\
do {\
	(_disk_cmd_)->ch = (_ch_);\
	(_disk_cmd_)->ch_ctx = (_ch_ctx_);\
	nvmeibc_disk_cmd_status_debug((_disk_cmd_), NVMEIBC_DISK_CMD_REMOTE_GOT_CH);\
} while(0)

#define nvmeibc_disk_cmd_status_debug_init(_disk_cmd_, _disk_)\
do {\
		memset((_disk_cmd_)->pre_poison, 0xaa, ARRAY_SIZE((_disk_cmd_)->pre_poison)); \
		memset((_disk_cmd_)->post_poison, 0xee, ARRAY_SIZE((_disk_cmd_)->post_poison)); \
		(_disk_cmd_)->ch = NULL;\
		(_disk_cmd_)->ch_ctx = NULL;\
		(_disk_cmd_)->disk = (_disk_);\
		(_disk_cmd_)->init_cpu = smp_processor_id();\
		if ((_disk_cmd_)->uncompleted_list_n.prev || (_disk_cmd_)->uncompleted_list_n.next)\
			BUG_ON(!list_empty(&(_disk_cmd_)->uncompleted_list_n));\
		INIT_LIST_HEAD(&(_disk_cmd_)->uncompleted_list_n);\
		nvmeibc_disk_cmd_status_debug((_disk_cmd_), NVMEIBC_DISK_CMD_INIT);\
} while(0)

#else //NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED

#define nvmeibc_disk_cmd_status_debug_init(_disk_cmd_, _disk_) do {\
		(void)_disk_cmd_;\
		(void)_disk_;\
	} while (0)

#define nvmeibc_disk_cmd_status_debug_got_ch(_disk_cmd_, _ch_, _ch_ctx_) do {\
	(void)_ch_;\
	(void)_ch_ctx_;\
	(void)_disk_cmd_;\
	} while (0)

#define nvmeibc_disk_cmd_status_debug_got_remote_ch(_disk_cmd_, _ch_, _ch_ctx_) do {\
		(void)_ch_;\
		(void)_ch_ctx_;\
		(void)_disk_cmd_;\
	} while (0)

#define nvmeibc_disk_cmd_status_debug(_disk_cmd_, _status_) do {\
	(void)_disk_cmd_;\
	(void)_status_;\
} while (0)

#endif //NVMEIBC_DISK_CMD_DEBUG_UNCOMPLETED

#endif
