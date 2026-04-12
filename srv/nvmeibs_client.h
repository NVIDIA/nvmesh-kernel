/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_CLIENT_H
#define NVMEIBS_CLIENT_H

#include "kr_incs.h"
#include "nvmeibs_net.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_disk.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibs_types.h"
#include "nvmeib_public.h"
#include "nvmeib_version_shared.h"

#include "nvmeibs_disk_locks.h"
#include "nvmeibc_types.h"
#include "nvmeib_ib_driver.h"
#include "nvmeib.h"
#include "nvmeib_types.h"
#include "../toma/clnt/nvmeibt_client_protocol.h"
#include "nvmeibs_client_db.h"
#include "nvmeib_vex.h"
#include "nvmeibs_async_cookies.h"

enum {
	/* Size of Admin Channel RQ - If SRQ is not being used. */
	NVMEIBS_ADMIN_RQ_SIZE = 16
};

struct nvmeibs_ib_port;
struct nvmeibs_disk_info;

/* remote access info to metadata buffer
   per iocmd (nordda) or ioch (rdda) */
struct nvmeibs_io_metadata {
    void *virt;
    dma_addr_t phys;
    u32 size;
    u32 lkey;
    u32 rkey;
};


struct nvmeibs_rionic_ka {
	struct mutex lock;
	int n_ioch;
	bool dying;
	struct nvmeibs_ib_port *ib_port;
	/* updated by io-ch on path */
	struct nvmeib_alloc_n_map ka_map;
	u64 *ka_ctr;
	u64 ka_ctr_ioaddr;
	u32 ka_ctr_rkey;
	u64 ka_ctr_phys_addr;
	/* used to determine if ka has timed out */
	u64 last_ka_ctr;
	u64 last_ka_update_jif;
};

struct nvmeibs_rionic {
	struct nvmeibs_lionic *lionic;
	union ib_gid gid;
	struct list_head link;
	int n_nr_channels;
	struct nvmeibs_nr_channel *nr_channels;
	/* true if rionic's gid was reported as
	   active by the client in acc-map req.
	   Non-active gids are kept in the map
	   so the can be used by nordda channels
	   when they become active without doing
	   rediscovery */
	bool may_access;
	/* MR page size (bytes) for this client ionic; from access-map wire ext1 */
	u32 mr_page_size;
	struct nvmeibs_rionic_ka keep_alive;
};

struct nvmeibs_client_disk {
	struct nvmeibs_disk_info *di;
	struct list_head link;
	u64 id;
	struct page_list_info *pli;
	/* number of local nics the client uses */
	int n_lionics;
	struct nvmeibs_lionic *lionics;
	/* number of "resources" i.e. RDDA Qs sent to client */
	int n_disk_rsrc;
};

struct nvmeibs_ib_port;
struct nvmeibs_lionic {
	struct nvmeibs_client_disk *disk;
	union ib_gid gid;
	int n_rionics;
	struct nvmeibs_rionic *rionics;
	struct list_head link;
	/* local io nic hw type */
	u16 hw_type;
	/* true if lionic's gid was active when
	   server parsed client's acc-map req.
	   Non-active gids are kept in the map
	   so the can be used by nordda channels
	   when they become active without doing
	   rediscovery */
	bool may_access;
	struct nvmeibs_ib_port *port;
};

struct nvmeibs_anic {
	/* the gid */
	union ib_gid gid;
	/* number of disks from the controller the client access */
	int n_disks;
	struct nvmeibs_client_disk *disks;
	struct list_head link;
};

/* controller command */
struct nvmeibs_cmd_info {
	struct list_head link;
	bool has_rsp;
	enum nvmeibs_cmd_ops op;
};

struct nvmeibs_get_put_cmd {
	struct nvmeibs_cmd_info info;
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
};

static inline struct nvmeibs_get_put_cmd *get_pg_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(info, struct nvmeibs_get_put_cmd, info);
}

struct nvmeibs_put_cmd {
	struct nvmeibs_get_put_cmd cmd;
	__be64 ids[NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT];
	u64 n_ids;
};

static inline struct nvmeibs_put_cmd *get_put_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(get_pg_info(info), struct nvmeibs_put_cmd, cmd);
}

struct nvmeibs_get_cmd {
	struct nvmeibs_get_put_cmd cmd;
	u64 n;
};

static inline struct nvmeibs_get_cmd *get_get_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(get_pg_info(info), struct nvmeibs_get_cmd, cmd);
}

struct nvmeibs_abnd_free_cmd {
	struct nvmeibs_get_put_cmd cmd;
	u32 rng_num;
	u64 rng_gen_id;
	u32 n;
	DECLARE_BITMAP(abnd_free_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	struct nvmeib_jrnl_ent_md ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	binje_t binje;
};

static inline struct nvmeibs_abnd_free_cmd *get_abnd_free_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(get_pg_info(info), struct nvmeibs_abnd_free_cmd, cmd);
}

struct nvmeibs_gid_change_cmd {
	struct nvmeibs_get_put_cmd cmd;
	union ib_gid hw_gid;
	union ib_gid gid;
	enum rdma_link_layer layer;
	bool may_access;
};

static inline struct nvmeibs_gid_change_cmd *get_gid_change_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(get_pg_info(info), struct nvmeibs_gid_change_cmd, cmd);
}


struct nvmeibs_ioch_drained_cmd {
	struct nvmeibs_get_put_cmd cmd;
	union ib_gid s_hw_gid;
	union ib_gid c_hw_gid;
	bool is_rdda;
	u16 ch_num;
	u64 cs_gid;
};

struct nvmeibs_client_logout_msg {
	struct nvmeibs_get_put_cmd cmd; /* keep first */
	int reason;
};

static inline struct nvmeibs_ioch_drained_cmd *get_ioch_drained_info(
	struct nvmeibs_cmd_info *info)
{
	return container_of(get_pg_info(info), struct nvmeibs_ioch_drained_cmd, cmd);
};

enum nvmeibs_ka_state {
	KA_NOT_USED = 0,
	KA_INIT,
	KA_ENABLED,
	KA_DISABLED,
};

struct nvmeibs_2nd_lock_ch
{
	struct nvmeibs_net *net;
	struct nvmeibs_client *cl;
	int idx;
};

struct nvmeibs_disk_info;
struct nvmeibs_client {
	uuid_be client_uuid;
	/* the client name */
	char host_name[NVMEIB_HOST_NAME_LEN];
	/* the disk the client is accessing */
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	/* the client full name */
	char name[NVMEIBS_CLIENT_NAME_SIZE];
	/* set if client's requested (GET_IO) disk was found and 'linked' to  */
	bool disk_linked;
	struct nvmeibs_disk_info *di;

	/* the client version */
	union nvmeib_version c_version;
	/* link version */
	union nvmeib_version link_version;
	/* the client port */
	struct nvmeibs_ib_port *ib_port;
	/* the client unique id */
	u32 cid;
	/* indication that the client is local*/
	bool is_local;
	/* link to nvmeibs_client_ldisk_list */
	struct list_head ldisk_link;
	/* if register to this cl, clnt module's
	   local-disk signals its done via this */
	struct completion ldisk_done;
	/* the management qp */
	struct nvmeibs_net *net;
	/* rdma message buffer(s) */
	struct nvmeib_alloc_n_map in_msg_area_map;
	struct nvmeib_alloc_n_map out_msg_area_map;
	void *in_msg_area;
	void *in_msg_area_end;
	void *out_msg_area;
	void *out_msg_area_end;
	/* keep alive message buffer */
	struct nvmeib_hdr *ka_msg_area;
	u64 ka_msg_dma_addr;
	/* indicates that keep alive sent without completion. */
	atomic_t ka_sent;
	/* The time to throw keep alive timeout */
	unsigned long ka_timeout;
	/* enumerator for keep alive messages */
	u64 ka_id;
	/* keep alive state */
	enum nvmeibs_ka_state ka_state;
	spinlock_t ka_spinlock;
	/* number of messages from data channel on read */
	int n_msgs;
	/* the list of admin nics that the client accesses */
	struct list_head anics;
	/* the combined number of disks used by the client */
	int n_disks;
	//TODO: change from list to ptr
	struct list_head disks;
	/* the combined number of local nics used by the client */
	int n_lnics;

	u32 nrch_ioreq_num;
	u32 max_wrs_per_req;

	struct list_head lnics;

	/* a response buffer */
	struct volume_server_rsp rsp;

	/* true when client is in a taking down process */
	atomic_t dying;
	u64 dying_start_time;
	spinlock_t spinlock;

	/* pending commands */
	struct list_head pending_cmd;
	/* on the wire cmd */
	struct list_head wire_cmd;

	int client_io_cmd_n_pages;

	/* work queue for message handling */
	struct workq_struct *wq;
	struct nvmeib_ref msg_area_refcount;
	struct completion *release_done;
	struct nvmeibs_cdb_item cdb;

	/* disk name cache - for No-RDDA mode where we need to check the disk name
	   in context of interrupt
	*/
	//TODO: Remove this, unused
	struct nvmeibs_disk_info **disk_cache;
	int n_caches;

	struct nvmeibs_net *lock_net;
	struct nvmeibs_2nd_lock_ch _2nd_lock_ch[NVMEIB_N_2ND_LOCK_CHS];
	atomic_t n_2nd_lock_ch;

	/*indicates that the lock channel is validated, to eliminate possibility
	  of two net locks on the same client. */
	bool lock_validated;

	/*indicates that client is dissconnected*/
	bool dismissed;
	/* indicates that a DREP timeout occurred  */
	bool drep_timeout;

	atomic_t toma_conn_refcnt;

	struct ib_wc scq_wcs[NVMEIBS_POLL_SIZE];
	struct ib_wc rcq_wcs[NVMEIBS_POLL_SIZE];

	u32 jrnl_rng;
	binje_t jrnl_rng_binje;
	u32 jrnl_rng_n_ent;

	/* receive queue for when running without global SRQ */
	struct nvmeib_recvq *recv_q;

	struct completion *get_jmdc_comp;
	spinlock_t get_jmdc_comp_lock;

	int connected_to_toma;

	struct nvmeib_alloc_n_map atomic_test_zone_map;

	/* work queue for flushing cl->wq on removal,
	   mainly needed because of secondary lock-ch
	   release-wq which added in deferred work */
	struct workq_struct *remove_wq;

	const struct vex_ops *vex_ach_ops[vex_ach_ops_num];
	const struct vex_ops *vex_io_req_ops;
	const struct vex_ops *vex_io_rsp_ops;

	bool fwd2_pending_received_msgs;
	u32 recv_idx_head;
	u32 recv_idx_tail;

	/* iu for sending msg from Toma to Clnt ;
	   not used for internal srv->clnt TOMA_RSP */
	struct nvmeib_iu *toma_send_work_iu;
	bool toma_send_work_iu_inuse;

	struct nvmeibs_async_cookie_store cookie_store;

	/* snapshots of nvmeibs-max_nr_channels_per_path module-param */
	int max_nrchs_per_path_rdma;
	int max_nrchs_per_path_tcp;

       /* wr_id.index in NVMEIB_RDMA_GET_JMDC rdma-write */
	atomic_t write_get_jmdc_cnt;

	struct proc_dir_entry *proc_dir;
	struct nvmeib_public_procfs_ent *qp_stats;

	struct {
		struct completion *comp;
		spinlock_t comp_lock;
		int opcode;
		u64 tag_cntr;
	} clnt_toma_rsp;
};

struct alloc_work {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeibs_ib_port *port;
	struct nvmeib_rdma_cm *cm_id;
	struct nvmeibc_login_request req;
};

enum nvmeibs_client_ioch_type {
	NVMEIBS_IOCH_RDDA	= 0,
	NVMEIBS_IOCH_NORDDA	= 1
};

struct nvmeibs_dev;
int nvmeibs_client_allocate(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client **cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);

struct nvmeibs_create_ioch_work {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeib_rdma_cm *cm_id;
	struct nvmeibc_login_request req;
};

struct nvmeibs_free_cl_work {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	void (*free_cl_cb)(struct nvmeibs_client *);
	enum nvmeibs_logout_reason reason;
};

struct nvmeibs_handle_msg_work {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeib_iu *send_ioctx;
	struct nvmeib_iu *recv_ioctx;
};

struct nvmeibs_client_proc_work {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	char *buffer;
	size_t len;
	int *count;
	struct completion *comp;
};

/**
 * lock client intialization
 *
 * @author yaron (6/2/2015)
 *
 * @param ib_port
 * @param cm_id
 * @param cl
 * @param req
 * @param rej
 *
 * @return int
 */
int nvmeibs_client_connect_admin_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client*cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);
int nvmeibs_client_connect_lock_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client*cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);
int nvmeibs_client_connect_2nd_lock_ch(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client*cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);
int nvmeibs_client_connect_io_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);
int nvmeibs_client_release(struct nvmeibs_client *cl,
	void (*free_cl_cb)(struct nvmeibs_client *),
	enum nvmeibs_logout_reason);
struct nvmeib_iu *nvmeibs_client_get_ioctx(struct nvmeibs_client *cl);
void nvmeibs_client_put_ioctx(struct nvmeibs_client *cl,
	struct nvmeib_iu *ioctx);
int nvmeibs_client_add_work(struct nvmeibs_client *cl,
	struct workqe_struct *work);

struct cl_external_workq {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
};
int nvmeibs_client_add_work_external(u64 cid, struct cl_external_workq *ew);
void nvmeibs_client_put_resource(struct nvmeibs_client *cl,
	const char *disk_name, __be64 *rscs, u64 n);
void nvmeibs_client_get_resource(struct nvmeibs_client *cl,
	const char *disk_name, u64 n);
int nvmeibs_client_create_disk_cache(struct nvmeibs_client *cl,
	struct list_head *disks, int n);
void nvmeibs_client_keep_alive(struct nvmeibs_client *cl);
#if 0 /* DEBUG ONLY */
void nvmeibs_client_check_trigger(struct nvmeibs_client *cl);
#endif
int nvmeibs_client_send_msg(struct nvmeibs_client *cl, struct nvmeibs_net *net,
	struct nvmeib_iu *iu, int len, int wr_opcode, u16 wr_version);
int nvmeibs_client_send_rsp(struct nvmeibs_client *cl, struct nvmeibs_net *net,
	u8 opcode, u64 tag, __be16 version_tag, struct nvmeib_iu *send_ioctx, void *p, int len,
	int wr_opcode, u16 wr_version);
bool nvmeibs_client_check_lock( struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port);
int nvmeibs_client_connect_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej,
	workq_func_t connect_channel_work_f);

void nvmeibs_client_prp_io_channel_def(struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_ib_port *ib_port,
	struct nvmeibc_io_channel_def *def);
struct nvmeibs_rionic *nvmeibs_client_get_rionic(struct nvmeibs_client *cl,
	struct nvmeibc_io_channel_def *def, enum nvmeibs_client_ioch_type ioch_type);

/*print client information in json format*/
ssize_t nvmeibs_client_print_client_info(struct nvmeibs_client *cl,
	char *buffer, int len);

int nvmeibs_client_ldisk_register(u64 cid, const char *disk_name,
	struct nvmeib_local_disk *ldisk);
int nvmeibs_client_ldisk_unregister(u64 cid, struct nvmeib_local_disk *ldisk);
/* Called after local-channel was connected i.e. lock-dev refcnt inc by ldisk */
int nvmeibs_client_ldisk_alloc_locks(u64 cid, struct nvmeib_local_disk *ldisk);
/* Called by local-client to allocate journal range */
int nvmeibs_client_ldisk_alloc_jrnl_rng(u64 cid, struct nvmeib_local_disk *ldisk,
										const struct nvmeib_jrange_cache *jrc,
										binje_t binje_req);
/* Called by SERJIO to tell client that abandoned journal entries are now free for use */
int nvmeibs_client_send_journal_abnd_free(u64 client_id, const char *disk_name,
					  u32 rng_num, u64 rng_gen_id, unsigned long *abnd_free_bitmap,
					  struct nvmeib_jrnl_ent_md *ent_md, binje_t binje);

int nvmeibs_client_update_gid_change(struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port);
int nvmeibs_client_stop_io_on_lgid(struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port);

int nvmeibs_client_check_all_io_paths_ka(struct nvmeibs_client *cl);
int nvmeibs_client_disconnect_io_path(struct nvmeibs_client *cl,
									  struct nvmeibs_rionic *rionic);

int nvmeibs_client_rionic_fill_rsp_io_ka(struct nvmeibs_rionic *rionic,
										 struct nvmeibs_ib_port *ib_port,
										 struct nvmeibs_login_response *rsp);

void nvmeibs_client_rionic_disconnect_ioch(struct nvmeibs_rionic *rionic, bool io_ka_enabled);
void nvmeibs_client_rionic_check_ka(struct nvmeibs_rionic *rionic);

void nvmeibs_client_send_ioch_drained(struct nvmeibs_client *cl, bool is_rdda,
	struct nvmeibs_rionic *rionic, int ch_num, u64 cs_gid);
int nvmeibs_client_logout(struct nvmeibs_net const *net);

struct proc_dir_entry *nvmeibs_client_proc_mkdir(struct proc_dir_entry *parent);
void nvmeibs_client_proc_umkdir(struct proc_dir_entry *parent);
#endif
