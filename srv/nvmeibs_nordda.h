#ifndef NVMEIBS_NORDDA_H
#define NVMEIBS_NORDDA_H

#include "nvmeibs_client.h"
#include "nvmeibs_net.h"
#include "nvmeibs_types.h"

#define NR2C(_nrch_) (_nrch_->net->params.cl)
#define NR2P(_nrch_) (_nrch_->net->params.port)

#if NVMEIBS_NR_LAT_MEAS
struct nvmeibs_nr_cmd_lat_meas {
	ktime_t submit_time;
	ktime_t comp_time;
	ktime_t post_send_time;
};

struct nvmeibs_nrch_stats_per_cpu {
	u64 n_iops_submit;
	u64 n_iops_comp;
	u64 n_iops_err;
	u64 tot_lat_us;
	unsigned int min_lat_us;
	unsigned int max_lat_us;
	u64 tot_recv_poll_lat_us;
	unsigned int min_recv_poll_lat_us;
	unsigned int max_recv_poll_lat_us;
	u64 tot_recv_submit_lat_us;
	unsigned int min_recv_submit_lat_us;
	unsigned int max_recv_submit_lat_us;
	u64 tot_send_time_us;
	unsigned int min_send_time_us;
	unsigned int max_send_time_us;
	u64 tot_ack_time_us;
	unsigned int min_ack_time_us;
	unsigned int max_ack_time_us;
	u64 tot_send2comp_time_us;
	unsigned int min_send2comp_time_us;
	unsigned int max_send2comp_time_us;
	u64 tx_cpu_cnt[NVMEIB_DFLT_MAX_CPUS];
	u64 rx_cpu_cnt[NVMEIB_DFLT_MAX_CPUS];
	u64 rx_queue_cnt[NVMEIB_DFLT_MAX_CPUS];
	u32 rx_skb_hash;
};

struct nvmeibs_nrch_lat_meas {
	/* SIW WC MD */
	void *scq_wcs_siw_md;
	void *rcq_wcs_siw_md;

	/* For Proc FS */
	struct proc_dir_entry *parent_proc_dir;
	struct proc_dir_entry *proc_dir;
	struct nvmeib_public_procfs_ent *stats_proc_file;
	
	struct nvmeibs_nrch_stats_per_cpu __percpu *stats_per_cpu;
};
#else
struct nvmeibs_nr_cmd_lat_meas {};
struct nvmeibs_nrch_lat_meas {};
#endif

/* nordda io command */
struct nvmeib_alloc_n_map;

#define CMD_STATS_HIST_LEN 4

struct nvmeibs_nr_cmd_stats_md {
	u16 version;
	u32 ch_version;
	ktime_t send_rsp_time;
	size_t send_rsp_len;
	size_t io_xfer_size;
	unsigned stat_verb;
	bool valid;
};

struct nvmeibs_nr_cmd {
	unsigned idx;
	struct nvmeibs_nvme_req req;
	__be64 req_hdr_tag;
	__be16 req_version_tag;
	union {
		struct volume_client_io_req io_req;
		struct volume_client_gen_req gen_req;

		struct volume_client_io_req_max_DONT_USE io_req_DONT_USE;
	} *cmd_req;
	struct nvmeib_iu *recv_ioctx;
	size_t cmd_req_sz;
	
	struct nvmeib_iu *io_iu;
	/* 8:  data wrs and io-rsp wr
	   1:  metadata wr */
	struct nvmeib_send_wr wr[8 + 1];
	struct nvmeib_alloc_n_map_info io_cmd_mem;
	struct nvmeib_alloc_n_map *bb_map;
	struct page *bb_prpl_page;
	struct nvmeibs_io_metadata md;

	struct {
		struct completion *done;
		int status;
	} gen_disk_cmd;

	/* nordda channel command belongs to */
	struct nvmeibs_nr_channel *nrch;

	bool gen_cmd;
	union {
		enum nvmeibc_indirect_op ind_op;
		enum nvmeib_gen_cmd_op gen_op;
	};

#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
	/* Catch double callback from the disk */
	atomic64_t n_submit, n_cb;
	unsigned long cb_ents[5];
#endif

	u16 version;
	u32 ch_version;
	
	atomic_t n_send_rsp_io;
	atomic_t n_send_rsp_io_comp;

	struct nvmeibs_nr_cmd_lat_meas lat_meas;

	struct workqe_struct work;

	/* Metadata for nic io-stats, we need to keep a small history
	  Because the send completion can be delayed */
	struct nvmeibs_nr_cmd_stats_md stats_md_ring[CMD_STATS_HIST_LEN];
	u16 stats_md_prod;
};

#define CMD_STATS_MD_HEAD(cmd) (cmd->stats_md_ring[cmd->stats_md_prod % CMD_STATS_HIST_LEN])

struct nvmeibs_nr_channel;
struct gens_cmd_ctx;

struct nvmeibs_nr_channel {
	char name[NVMEIBS_CLIENT_NAME_SIZE + 1 +
		NVMEIBS_CONNECTION_LOCAL_NAME_SIZE];
	int id;
	u64 cs_gid;
	u64 cs_gid_last_sent;
	struct nvmeibs_rionic *rionic;
	struct nvmeibs_net *net;
	/* private 'shared' recvq */
	struct nvmeib_srq_info *priv_srq;
	/* receive queue for when srq is not used */
	struct nvmeib_recvq *recv_q;

	spinlock_t spinlock;

	unsigned int n_scq_wcs;
	unsigned int n_rcq_wcs;

	struct ib_wc *scq_wcs;
	struct ib_wc *rcq_wcs;

	/* deferred io cmds wq (custom nvmeib_q, used when kernel wq is disabled) */
	struct workq_struct *wq;
	/* io cmds */
	int io_cmd_n_pages;
	struct nvmeibs_nr_cmd *io_cmds; /* [ up to NVMEIB_MAX_NORDDA_IO_REQ] */
	atomic_t underway_cmds;
	struct completion *underway_cmds_comp;

#ifdef NVMEIBS_NR_CATCH_CMD_ALREADY_UNDERWAY
	DECLARE_BITMAP(underway_cmds_bmp, NVMEIB_MAX_NORDDA_IO_REQ);
#endif

	/* VEX */
	const struct vex_ops *io_srv_lock_req_ops;
	const struct vex_ops *io_srv_lock_rsp_ops;
	const struct vex_ops *gen_br_ops;
	const struct vex_ops *gen_uj_ops;
	const struct vex_ops *gen_db_ops;
	const struct vex_ops *gen_fje_ops;
	const struct vex_ops *gen_je_ops;
	const struct vex_ops *gen_fje_ent_ops;

	/* for debug */
	int max_send_wrs;

	/* Defer SRQ-Post-Recv of io-req until send completion of io-rsp */
	u32 n_rxiu;
	u64 n_rxiu_tot;
	bool rxiu_dying;
	struct list_head rxiu_list;
	
	void *jrange_handle;
	void *jmdc_map_handle;

	/* For operations that use async cookies */
	struct nvmeibs_async_cookie_channel_data *cookie_ch;
	
	struct nvmeibs_nrch_lat_meas lat_meas;

	/* Deferred recv completion work */
	struct work_struct recv_comp_work;
	atomic_t recv_comp_work_ctr;
};

struct nvmeibs_nr_channel *nvmeibs_nordda_alloc_channel(void);
int nvmeibs_nordda_connect_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej);

int nvmeibs_nordda_add_work(struct nvmeibs_nr_channel *nrch,
	struct workqe_struct *work);

int nvmeibs_nordda_io_cmds_alloc(struct nvmeibs_nr_channel *nrch);
int nvmeibs_nordda_fill_config_alloc_nr_net_rsp(struct nvmeibs_nr_channel *nrch,
	struct volume_server_config_alloc_nr_net_rsp *rsp);

extern bool nvmeibs_nordda_use_kernel_wq;
extern bool nvmeibs_nordda_kernel_wq_unbound;
extern struct workqueue_struct *nvmeibs_nordda_kwq;

int nvmeibs_nordda_kwq_init(void);
void nvmeibs_nordda_kwq_exit(void);
void nvmeibs_nordda_kwq_flush(void);

#endif

