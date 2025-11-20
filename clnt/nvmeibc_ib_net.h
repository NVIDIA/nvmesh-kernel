#ifndef NVMEIBC_IB_NET_H
#define NVMEIBC_IB_NET_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeibc_types.h"
#include "nvmeib_srq.h"
#include "nvmeib_version_shared.h"
#include "nvmeibc_core_dbgdi_shared.h"
#include "nvmeib_public.h"
#include "nvmeib_ib_driver.h"

enum {
	NVMEIBC_IB_PORT_REDIRECT = 1,
	NVMEIBC_IB_DLID_REDIRECT = 2,
	NVMEIBC_IB_STALE_CONN = 3,
};

enum nvmeibc_ib_net_state {
	NVMEIBC_IB_NET_INIT,
	NVMEIBC_IB_NET_CONNECTING,
	NVMEIBC_IB_NET_LIVE,
	NVMEIBC_IB_NET_DISCONNECTING,
	NVMEIBC_IB_NET_REMOVED,
	NVMEIBC_IB_NET_ERR,
};

const char *nvmeibc_ib_net_state_str(enum nvmeibc_ib_net_state);

/* remove net work */
struct remove_net_workq {
	struct workqe_struct work;
	bool defer_release_wq_create;
};

enum nvmeibc_map_sg_modes {
	MAP_SG_MR_COMBINED 	  = 0,

	/* less performance, more data protection
	   use case: debug rdma-remote-mem-corruption */
	MAP_SG_MR_REG_WR_ONLY = 1,

	/* more performance, less data protection
	   map-sg() will NOT use IB_WR_REG_MR and each sg-entry
	   will use a dedicated rdma-write WR from/to client
	   use case: azure IB cluster with 8k < bs < 32k  */
	MAP_SG_MR_GLOBAL_ONLY = 2
};

#define NVMEIBC_IB_NET_WC_ARR_SIZE NVMEIB_MAX_NORDDA_IO_REQ

/**
 * struct nvmeibc_map_state - per-request DMA memory mapping
 * state
 * @desc:	    Pointer to the element of the host buffer
 *  		descriptor array that is being filled in.
 * @pages:	    Array with DMA addresses of pages being considered for
 *		    memory registration.
 * @base_dma_addr:  DMA address of the first page that has not yet been mapped.
 * @dma_len:	    Number of bytes that will be registered with the next
 *		    FMR or FR memory registration call.
 * @total_len:	    Total number of bytes in the sg-list being mapped.
 * @npages:	    Number of page addresses in the pages[] array.
 * @nmdesc:	    Number of FMR or FR memory descriptors used for mapping.
 * @ndesc:	    Number of host buffer descriptors that have been
 *  	 filled in.
 * @unmapped_sg:    First element of the sg-list that is mapped via FMR or FR.
 * @unmapped_index: Index of the first element mapped via FMR or FR.
 * @unmapped_addr:  DMA address of the first element mapped via FMR or FR.
 */
struct nvmeibc_map_state {
	union {
		struct ib_pool_fmr **next_fmr;
		struct nvmeib_fr_desc **next_fr;
	};
	struct nvmeib_direct_buf *desc;
	u64 *pages;
	dma_addr_t base_dma_addr;
	u32 dma_len;
	u32	total_len;
	unsigned npages;
	unsigned nmdesc;
	unsigned ndesc;
	struct scatterlist *unmapped_sg;
	int unmapped_index;
	dma_addr_t unmapped_addr;
	bool allow_dma_key;
};

struct ib_pool_fmr;
struct nvmeib_fr_desc;
struct nvmeib_direct_buf;
struct nvmeibc_disk_io_command;
struct nvmeib_iu;

struct nvmeibc_volume_request {
	struct list_head link;
	union {
		struct ib_pool_fmr **fmr_list;
		struct nvmeib_fr_desc **fr_list;
	};
	u64 *map_page;
	struct nvmeib_direct_buf *indirect_desc;
	struct nvmeib_direct_buf table_desc;
	/* request dma direction */
	enum dma_data_direction dma_dir;
	dma_addr_t indirect_dma_addr;
	u32 total_dma_len; 	/* assigns io-req's indirect_hdr->len and io_req->data_len and */
	u32 ndesc;			/* assigns io-req's indirect_hdr->table_count (and used by for loops...) */
	short nmdesc;		/* num of map-mr descs used to issue IB_WR_REG_MR (or IB_WR_FAST_REG_MR) */
	short sgcount;		/* used by map-data logic to branch btw one ndesc or more + to do unmap */
	short pgcount;
	s16 index;
	union {
		struct nvmeibc_disk_command *dcmd;
		struct nvmeibc_disk_io_command *bcmd;
	};
	struct nvmeib_iu *cmd;
	struct nvmeib_iu *reuse_cmd;
	/* MD related */
	/* DMA map of block's io-req's metadata (@iocmd->reqs[0].md) */
	struct {
		bool has;
		bool has_inline;
		bool is_dummy;
		int entry_size;
		void *data;
		struct {
			u64 addr;
			u32 size;
			u32 lkey;
			u32 rkey;
		} local, remote;
	} md;
	int bb_pages;
	/* should be either 0 or 1 ;
	   inc/dec by init_reuse_request/del_reuse_request */
	int reused_bb;
	int reused_bb_last_del_tag;
	/* reused-bb (aka rcookie) lru */
	struct list_head reused_bb_lru_link;
	unsigned long    reused_bb_lru_jif;

#if	defined(DEBUG_REQ_REUSED_BB_STATE) && (DEBUG_REQ_REUSED_BB_STATE == 1)
	struct nvmeib_state_guard reused_bb_state;
#endif

	/* core-dbgdi stuff */
	#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#else
		/* lock-piggyback - currently covering ONLY remote-write-piggyb */
		struct t_core_dbgdi_lock_piggyback lock_pgbk;
	#endif // DBGDI_REMOVED_IN_PRODUCTION

	/* have we poisoned remote bb before posting read (wr) of this req */
	bool rd_pois;			// Todo: #ifdef me

	enum nvmeibc_map_sg_modes map_sg_mode;
};

#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#define nvmeibc_ib_net_vol_req_dbgdi_reset(...)
	#define nvmeibc_ib_net_vol_req_dbgdi_set(...)
	#define nvmeibc_ib_net_vol_req_dbgdi_check_rsp(...)
#else
/* called on start of (RDDA/No-RDDA) execute-io */
#define nvmeibc_ib_net_vol_req_dbgdi_reset(__req) 	\
do {												\
	__req->lock_pgbk.valid = false;					\
} while (0)

/* called on when filling WR (RDDA) or io_req (No-RDDA) */
#define nvmeibc_ib_net_vol_req_dbgdi_set(__req,  	\
	__type, __addr, __val0, __val1, __off, __val)	\
do {												\
	__req->lock_pgbk.ulp.type = __type;				\
	__req->lock_pgbk.ulp.addr = __addr;				\
	__req->lock_pgbk.ulp.val0 = __val0;				\
	__req->lock_pgbk.ulp.val1 = __val1;				\
	__req->lock_pgbk.llp.off = __off;				\
	__req->lock_pgbk.llp.val = __val;				\
	__req->lock_pgbk.valid = true;					\
} while (0)

//TBD: panic if comp_code == 0
/* called on when processing io-rsp (No-RDDA only) */
#define nvmeibc_ib_net_vol_req_dbgdi_check_rsp(__req,					\
	__rsp_lockent, __comp_code)											\
do {																	\
	if (__req->lock_pgbk.valid &&										\
		__req->lock_pgbk.llp.val != __rsp_lockent->blkset_info.all) {	\
		_NE(err_nvmeibc_ib_net_vol_req_dbgdi_check_rsp,					\
			"OOPS, write-binfo: ulp={@INT64_HEX, @INT64_HEX}, "			\
			"llp=@INT64_HEX, rsp=@INT32_HEX, comp_code=@INT32_HEX",		\
			__req->lock_pgbk.ulp.val0, __req->lock_pgbk.ulp.val1,		\
			__req->lock_pgbk.llp.val, __rsp_lockent->blkset_info.all,	\
			__comp_code);												\
	}																	\
} while (0)
#endif 	// DBGDI_REMOVED_IN_PRODUCTION

struct nvmeibs_login_response;
struct nvmeibs_login_reject;
struct nvmeibc_admin_channel;
struct nvmeibc_ib_net;
struct nvmeibc_channel;
struct nvmeib_srq_info;
/* the ib net params */
struct nvmeibc_ib_net_params {
	struct nvmeibc_ib_port *port;
	struct ib_sa_path_rec *path;
	/* well one always needs a lock... */
	spinlock_t spinlock;
	int max_send_q;
	int max_recv_q;
	int max_send_sg;
	int max_recv_sg;
	int max_send_cq;
	int max_recv_cq;
	int recv_msg_size;
	bool use_srq;
	int max_rd_atomic; /* Max outgoing Read/Atomic Ops */
	int max_dest_rd_atomic; /* Max incoming Read/Atomic Ops (to request from acceptor) */
	/* channel's private SRQ, valid if @use_srq == true */
	struct nvmeib_srq_info *srq_priv;
	/* get public SRQ, valid if @use_srq == true && @srq_priv == NULL */
	enum nvmeib_srq_type srq_type;
	bool use_atomic;
	bool free_net_on_connect_err;
	//void (*send_completion)(struct ib_cq *cq, void *context);
	//void (*recv_completion)(struct ib_cq *cq, void *context);
	int (*on_connected)(struct nvmeibc_ib_net *net);
	void (*on_connected_clear)(struct nvmeibc_ib_net *net);
	void (*on_remove_work)(struct nvmeibc_ib_net *net);
	int (*on_login)(struct nvmeibc_ib_net *net,
					struct nvmeibs_login_response *lrsp, int n_ext);
	void (*on_reject)(struct nvmeibc_ib_net *net,
					  struct nvmeibs_login_reject *rej);
	void (*on_disconnect)(struct nvmeibc_ib_net *net);
	void (*on_free)(struct nvmeibc_ib_net *net);
	void (*on_poison)(struct nvmeibc_ib_net *net);

	//void (*on_send_comp)(struct ib_cq *cq, void *net_ptr);
	//void (*on_receive_comp)(struct ib_cq *cq, void *net_ptr);
	int (*call_send_comp_handler)(struct nvmeibc_ib_net *net, struct ib_wc *wc,
								  bool last_in_series);
	int (*call_receive_comp_handler)(struct nvmeibc_ib_net *net,
									 struct ib_wc *wc);
	int rcq_offload_enb;
	int scq_offload_enb;
	
	int rcq_offload_cpu;
	int scq_offload_cpu;

	/*will loop for all interrupts*/
	bool poll_interrupts;

	/* pointer to channel RQ, if no SRQ */
	struct nvmeib_recvq *recv_q;

	/* send and recv shared CQ */
	bool shared_cq;

	/* defer receive interrupt handling */
	struct workq_struct *defer_recv_intr_wq;
	
	/* allocate metadata for SIW WCs */
	bool alloc_siw_wc_md;
	
	unsigned ch_index;
	
	bool nr_defer_recv_comps;

	/* if != NVMEIBC_COMP_CPU_INVALID && use-pcpu-cq is enabled, select cq of this cpu */
	int comp_cpu;
};

struct cq_stats {
	long last_time;
	/* num send comps processed while in interrupt mode */
	u64 n_intr;
	/* num send comps processed while in polling mode */
	u64 n_poll;
	/* num wakeups of polling kthread */
	u64 n_wakeups_burst;
	u64 n_wakeups_cycles;
	u64 n_wakeups_irq_time;
	/* num send comps their processing was not done directly
	   by interrupt or polling-kthread e.g. when post-send
	   fails we try to process scq to get free send iu */
	u64 n_external;
	u64 max_intr_duration;
	u64 n_spurious_intr;
	u64 n_missed_events;
	/* stats for defer to work-queue processing */
	u64 n_defer_wq;
	u64 n_defer_wq_already;
	u64 n_defer_poll;
	u64 n_defer_over_budget;
	u64 n_defer_missed_events;
	u64 n_defer_empty_cq;
	/* stats for per-cpu channels */
	u64 n_ipi_func;
};

#define STATS_FIELD(_A) _A##_stats
#define inc_spurious_intr_and_report(_net, cq_type) \
do { \
	if (((_net)->STATS_FIELD(cq_type).n_spurious_intr++ & 0xff) == 0) \
		_NW(NVMEIB_CONCAT2(inc_spurious_intr_and_report_, NVMEIB_CONCAT2(cq_type, __AUTOID__)), \
		 "@NET had @LU n_spurious_intr" , (_net), (_net)->STATS_FIELD(cq_type).n_spurious_intr); \
} while(0)

#define inc_rcq_spurious_intr_and_report(_net) inc_spurious_intr_and_report(_net, rcq)
#define inc_scq_spurious_intr_and_report(_net) inc_spurious_intr_and_report(_net, scq)

enum cq_poll_mode {
	NVMEIBC_IB_CQ_INTR = 0,
	NVMEIBC_IB_CQ_POLLING = 1,
	/* interrupt received after polling kthread have rearmed interrupts
	   but still had not changed cq-poll-mode to NVMEIBC_IB_CQ_INTR */
	NVMEIBC_IB_CQ_KEEP_POLLING = 2,
};

static inline const char *cq_poll_mode_to_str(int mode)
{
	switch (mode) {
	case NVMEIBC_IB_CQ_INTR: return "INTR";
	case NVMEIBC_IB_CQ_POLLING: return "POLLING";
	case NVMEIBC_IB_CQ_KEEP_POLLING: return "KEEP_POLLING";
	default: return "???";
	}
}

#define SCQ_OFFLOAD_TRACE 0
#if SCQ_OFFLOAD_TRACE
#define SCQ_OFFLOAD_TRACE_SIZE 10
#endif

enum nvmeibc_ib_net_defer_recv_state {
	NVMEIBC_IB_NET_DEFER_RECV_DISABLED = 0,
	NVMEIBC_IB_NET_DEFER_RECV_IDLE,
	NVMEIBC_IB_NET_DEFER_RECV_SCHEDULED,
	NVMEIBC_IB_NET_DEFER_RECV_RUNNING,
	NVMEIBC_IB_NET_DEFER_RECV_TERMINATING,
};

struct nvmeibc_ib_net_pcpu_call_data {
	atomic_t call_pending;
	call_single_data_t call_data;
};

struct nvmeibc_ib_net {
	struct nvmeibc_admin_channel *admin_ch;
	struct nvmeibc_ib_port *port;
	struct ib_sa_path_rec path;
	/* fat access for global mem keys */
	u32 lkey;
	u32 rkey;
	__be64 service_id;
	__be16 pkey;
	__be16 service_port;
	struct ib_cq *send_cq;
	struct ib_cq *recv_cq;
	struct nvmeib_recvq *recv_q;
	struct nvmeib_srq_info *srq_info;
	struct ib_qp *qp;
	u32 remote_qpn;
	u32 sq_psn;
	u8 max_rd_atomic;
	u8 max_dest_rd_atomic;
	u32 wd_timeout_jif;
	struct nvmeib_rdma_cm *cm_id;
	int cm_rdma_type;
	bool rearm_send_cq;
	struct nvmeib_state_guard state;
	int status;
	/* use long work when handle transport layer errors */
	struct remove_net_workq remove_work;
	/* call on connected */
	int (*on_connected)(struct nvmeibc_ib_net *net);
	void (*on_connected_clear)(struct nvmeibc_ib_net *net);
	/* call on remove_work */
	void (*on_remove_work)(struct nvmeibc_ib_net *net);
	int (*on_login)(struct nvmeibc_ib_net *net,
					struct nvmeibs_login_response *lrsp, int n_ext);
	void (*on_reject)(struct nvmeibc_ib_net *net,
					  struct nvmeibs_login_reject *rej);
	void (*on_disconnect)(struct nvmeibc_ib_net *net);
	void (*on_free)(struct nvmeibc_ib_net *net);
	void (*on_poison)(struct nvmeibc_ib_net *net);

	//int (*on_send_comp)(struct ib_cq *cq, void *net_ptr);
	//int (*on_receive_comp)(struct ib_cq *cq, void *net_ptr);
	int (*call_send_comp_handler)(struct nvmeibc_ib_net *net, struct ib_wc *wc, bool last_in_series);
	int (*call_receive_comp_handler)(struct nvmeibc_ib_net *net,
									 struct ib_wc *wc);
	/* pointer to device specific post_send_atomic fn */
	int (*post_send_atomic_fn)(struct ib_qp *qp, struct nvmeib_send_wr *send_wr,
				 struct nvmeib_send_wr **bad_send_wr);
	/* pointer to device specific peek_cq fn */
	int (*peek_cq)(struct ib_cq *ib_cq, int max);
	/*will loop for all interrupts*/
	bool poll_interrupts;
	/* completions */
	spinlock_t comp_guard;
	struct completion connect_done;
	struct completion *break_qp;
	struct completion wait_for_drep;
	bool peer_disconnected;
	int qp_rq_drain_recv;
	int qp_evt_last_wqe;
	int qp_evt_qp_fatal;
	/* 1 if we are in the process of disappearing */
	atomic_t dying;
	/* net owner private data */
	struct nvmeibc_channel *ioch;
	struct list_head link;
	/* completion array */
	struct ib_wc *wc_s;
	struct ib_wc *wc_r;
	struct ib_wc *wc_mixed;

	int n_wc_s;
	int n_wc_r;
	int n_wc_mixed;
	
	/* completion md for SIW */
	void *wc_md_s;
	void *wc_md_r;
	void *wc_md_mixed;

	/* recv cq poll mode */
	struct task_struct *rcq_kthread;
	enum cq_poll_mode rcq_poll_mode;
	struct completion rcq_kth_ready;

	/* send cq poll mode */
	struct task_struct *scq_kthread;
	enum cq_poll_mode scq_poll_mode;
	struct completion scq_kth_ready;
#if SCQ_OFFLOAD_TRACE
	struct {
		int cnt;
		int buf[SCQ_OFFLOAD_TRACE_SIZE];
		enum cq_poll_mode mode[SCQ_OFFLOAD_TRACE_SIZE];
		int printed;
	} scq_offload_trace;
#endif

	/* send comp stats */
	struct cq_stats scq_stats;
	/* recv comp stats */
	struct cq_stats rcq_stats;

	/* Counter for WQEs posted to RQ (if no SRQ) (num posted + 1) */
	atomic_t rq_post_count;
	struct completion *drain_sq_done;

	unsigned char gen;	// object generation - increased every time
				// net object is reused

	bool shared_cq;
	struct workq_struct *defer_recv_intr_wq;
	struct nvmeib_state_guard defer_recv_state;

	struct {
		void *src;
		size_t src_len;
		u64 src_dma;
		bool in_progress;
		atomic64_t sent;
	} io_ka;

	/* Use dev's percpu CQ pool */
	struct nvmeib_dev_cq *dev_cq;
	struct list_head qp_action_list;

	struct workqe_struct defer_recv_work;

	struct nvmeib_ref ib_rsrc_ref;

	struct nvmeib_intr_shaper *intr_shaper;

	/* Debug "Fail to drain SQ"...
	   Comp dose not arrive --> WD --> Fail to Drain SQ -->
	   Poll-CQ --> Original CQE and comp of drain-wqe are there...
	   Looking at /proc/interrupts, the EQ that uses @intr_vec
	   does NOT get any interrupts (anymore), suspecting pci rescan.
	*/
	u64 n_recv_intrs;
	u64 n_send_intrs;
	int recv_intr_vec;
	int send_intr_vec;

	struct nvmeib_rdma_evt_ctx *rdma_e_ctx;

	/* Cache of mod param */
	bool nr_defer_recv_comps;

#if defined(NVMEIBC_READ_POISON_BB) && (NVMEIBC_READ_POISON_BB==1)
	/* write/poison remote bb before read from disk */
	struct {
		struct nvmeibc_poison_area_header {
			uuid_be uniq; 	/* For uniqueness across clients */
			u64 tsc; 		/* For uniqueness across IOs */
		} *hdr; 			/* Points to &pois_src.vaddr */
		void *vaddr;
		int n_pages;
		struct ib_sge sge;
	} pois_src;

	struct {
		int opcode;
		u64 dlba;
		u64 len;
	} pois_prev_io;
#endif

	struct nvmeib_qp_stats_pcpu __percpu * qp_stats;

	/* Used for lock-less per-cpu channels without dev-cq */
	struct nvmeibc_ib_net_pcpu_call_data pcpu_send_comp_smp_call;
	struct nvmeibc_ib_net_pcpu_call_data pcpu_recv_comp_smp_call;
};

struct nvmeibc_ib_port;
int nvmeibc_ib_net_find_path(struct nvmeibc_ib_port *port,
	struct ib_sa_path_rec *path, bool read_source);
struct nvmeibc_login_request;

/**
 * establish a rdma connection to remote side according
 * to parameters
 *
 * @author ofer
 *
 * @param net will store the connection parameters
 * @param params parameters for connection
 * @param lreq credentials
 *
 * @return int success status
 */
int nvmeibc_ib_net_alloc(struct nvmeibc_ib_net *net,
	struct nvmeibc_ib_net_params *params,
	struct nvmeibc_login_request *lreq);

/**
 * close and free connection
 *
 *
 * @param net
 */
void nvmeibc_ib_net_free(struct nvmeibc_ib_net *net);

void nvmeibc_ib_net_break_qp(struct nvmeibc_ib_net *net);
void nvmeibc_ib_net_disconnect_(struct nvmeibc_ib_net *net);
void nvmeibc_ib_net_disconnect(struct nvmeibc_ib_net *net);
void nvmeibc_ib_net_destroy_cm(struct nvmeibc_ib_net *net);

int nvmeibc_ib_net_map_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int n_msgs);
int nvmeibc_ib_net_read_map_sg_to_ib_sge(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 read_from_addr, u32 lkey,
	int n_md_descs, int md_entry_size);
int nvmeibc_ib_net_write_map_sg_to_ib_sge(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 write_to_addr, u32 rkey);
int nvmeibc_ib_net_write_map_sg_to_ib_sge_extd_md(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 write_to_addr, u32 rkey,
	struct nvmeibc_volume_request *req, int io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift);
int nvmeibc_ib_net_build_wriu(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, struct nvmeib_iu *iu,
	enum nvmeib_block_io_op op, int *io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift,
	u64 raddr, u32 rkey);

/**
 * struct nvmeibc_ib_net   * __net
 * const struct ib_send_wr * __send_wr
 * const struct ib_send_wr __bad_send_wr
 */
#define nvmeibc_ib_post_send(__net, __send_wr, __bad_send_wr) ({	\
       int __rv;                                                    \
       nvmeib_qp_stats_on_post_send((__net)->qp_stats);        		\
       __rv = ib_post_send((__net)->qp, __send_wr, __bad_send_wr);  \
       __rv;                                                       	\
})

void nvmeibc_ib_net_complete_iocmd_sg_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int orig_sgcount, bool was_reuse);

#define nvmeibc_ib_net_complete_iocmd_sg(_n, _r) \
	nvmeibc_ib_net_complete_iocmd_sg_reuse(_n, _r, (_r)->sgcount, false)

void nvmeibc_ib_net_unmap_sg_to_ib_sge(struct nvmeib_iu *iu);
void nvmeibc_ib_net_complete_iocmd_block(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, enum stats_done_info_type done_type, int comp_code);

void nvmeibc_ib_net_complete_iocmd_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req,  enum stats_done_info_type done_type, int comp_code, int orig_sgcount, bool was_reuse);

#define nvmeibc_ib_net_complete_iocmd(_n, _r, _d, _c) \
	nvmeibc_ib_net_complete_iocmd_reuse(_n, _r, _d, _c, (_r)->sgcount, false)

void nvmeibc_ib_net_unmap_and_unlink_iocmd_reuse(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int comp_code, int orig_sgcount, bool was_reuse);

#define nvmeibc_ib_net_unmap_and_unlink_iocmd(_n, _r, _c) \
	nvmeibc_ib_net_unmap_and_unlink_iocmd_reuse(_n, _r, _c, (_r)->sgcount, false)

struct nvmeibc_dev;
void nvmeibc_ib_net_complete_bcmd(struct nvmeibc_disk_command *dcmd, enum stats_done_info_type done_type, struct nvmeibc_dev *local_dev);

void nvmeibc_ib_net_unmap_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req);
void nvmeibc_ib_net_free_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req);
int nvmeibc_ib_net_alloc_volume_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int index);
void nvmeibc_ib_net_free_volume_req(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req);
void nvmeibc_ib_net_get_bufs_nbufs(struct nvmeibc_volume_request *req,
	struct nvmeib_direct_buf **bufs, int *nbufs, int *len);
void nvmeibc_ib_net_print_riu(struct nvmeib_rdma_iu *riu);
void nvmeibc_ib_net_fill_wr(struct nvmeib_send_wr *wr, u64 id,
	struct nvmeib_rdma_iu *riu);
int nvmeibc_ib_net_post_recvq(struct nvmeibc_ib_net *net, struct nvmeib_recvq *rq, struct nvmeib_iu *iu);

int nvmeibc_ib_net_process_send_cq(struct nvmeibc_ib_net *net, int n);
void nvmeibc_ib_net_req_notify_send_cq(struct nvmeibc_ib_net *net);

int nvmeibc_ib_net_process_recv_cq(struct nvmeibc_ib_net *net, int n);
#ifdef DEBUG_CLNT_NET_STATS
void nvmeibc_ib_net_stats_columns(char *buf, size_t len, ssize_t *pcount);
void nvmeibc_ib_net_stats_fill_buf(struct nvmeibc_ib_net *net, int id,
								   char *buf, size_t len, ssize_t *pcount);
#endif
int nvmeibc_ib_net_intr_shaper_create(void);
void nvmeibc_ib_net_intr_shaper_destroy(void);

int nvmeibc_ib_net_execute_ka(struct nvmeibc_ib_net *net,
							  struct nvmeib_remote_access_info *rai,
							  void *src, size_t src_len);

/**
 * nvmeibc_ewrop(): encode wr op
 *
 */
static inline u64 nvmeibc_ib_net_ewrop(u16 op, u16 dir, u32 key)
{
	/*return nvmeib_encode_wr_id(((u32)op << 16) | dir, key);*/ (void)dir;
	return nvmeib_encode_wr_id(op, key);
}

bool nvmeibc_ib_net_drained(struct nvmeibc_ib_net *net);

static inline u64 nvmeibc_net_is_connected(struct nvmeibc_ib_net *net)
{
	int state = nvmeib_get_state_guard(&net->state);
	return state == NVMEIBC_IB_NET_CONNECTING || state == NVMEIBC_IB_NET_LIVE;
}

/* DMA map block's io-req metadata */
int nvmeibc_ib_net_map_md(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req, int io_len,
	unsigned sw_sc_shift, unsigned hw_sc_shift);
void nvmeibc_ib_net_unmap_md(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req);

enum reused_bb_state {
	REUSED_BB_UNUSED = 0xc0,
	REUSED_BB_TRY_USE,
	REUSED_BB_USED,
	REUSED_BB_TRY_END,
};

#if	defined(DEBUG_REQ_REUSED_BB_STATE) && (DEBUG_REQ_REUSED_BB_STATE == 1)
#define req_reused_bb_state_switch(_req, _o, _n) do {					 		\
	if (!nvmeib_switch_state_guard(&(_req)->reused_bb_state, _o, _n)) {			\
		pr_err("REQ_REUSED_BB: req=%p, cannot switch state %d -> %d, "			\
			   "curr-state=%d",													\
				_req, (int)_o, (int)_n,											\
			   nvmeib_get_state_guard(&(_req)->reused_bb_state));				\
		BUG();																	\
	}																			\
} while (0)

#define req_reused_bb_state_init(_req) do { \
		nvmeib_init_state_guard(&(_req)->reused_bb_state, REUSED_BB_UNUSED); \
} while (0)

void req_reused_bb_lru_init(struct nvmeibc_volume_request *req);
void req_reused_bb_lru_add(struct nvmeibc_channel *ch,
						   struct nvmeibc_volume_request *req);
void req_reused_bb_lru_del(struct nvmeibc_channel *ch,
						   struct nvmeibc_volume_request *req);
void req_reused_bb_lru_flush(struct nvmeibc_channel *ch);
void req_reused_bb_lru_is_timeout_stats(struct nvmeibc_channel *ch);

#else
#define req_reused_bb_state_switch(_req, _o, _n)
#define req_reused_bb_state_init(_req)
#define req_reused_bb_lru_init(_req)
#define req_reused_bb_lru_add(_ch, _req)
#define req_reused_bb_lru_del(_ch, _req)
#define req_reused_bb_lru_flush(_ch)
#define req_reused_bb_lru_is_timeout_stats(_ch)

#endif /* DEBUG_REQ_REUSED_BB_STATE */

#define req_reused_bb_check(_req, _exp) do {		\
	if ((_req)->reused_bb != _exp) {				\
		pr_err("REQ_REUSED_BB: req=%p, %d exp. %d", \
			   _req, (_req)->reused_bb, _exp);		\
		BUG();										\
	}												\
} while (0)

/*
   struct nvmeibc_volume_request --> 'int reused_bb'
   struct nvmeib_data_buffer     --> 'struct nvmeib_data_reuse_buf_params rcookie'
 */
#define do_reuse_request(req) \
	(get_rcookie_ptr((req)->bcmd)->action == nvmeib_data_reuse_buf_SAVE)

#define init_reuse_request(req, ch, req_i, __comp_cpu, __ch_ver, __disk_ver, disk) do {\
	struct nvmeib_data_reuse_buf_params *rcookie = get_rcookie_ptr((req)->bcmd);\
	BUG_ON((__disk_ver) == 0);\
	req_reused_bb_state_switch(req, REUSED_BB_UNUSED, REUSED_BB_TRY_USE);\
	req_reused_bb_check(req, 0);\
	if (rcookie->action != nvmeib_data_reuse_buf_SAVE) {\
		pr_err("REQ_REUSED_BB: req=%p, unexp action %d", req, rcookie->action);\
		BUG();\
	}\
	rcookie->req_id = (req_i);\
	rcookie->channel = (ch);\
	rcookie->channel_ver = (__ch_ver);\
	rcookie->disk_ver = (__disk_ver);\
	rcookie->comp_cpu = (__comp_cpu);\
	(req)->reused_bb = 1;\
	req_reused_bb_lru_add(ch, req);\
	req_reused_bb_state_switch(req, REUSED_BB_TRY_USE, REUSED_BB_USED);\
	nvmeibc_disk_inc_reuse((disk));\
} while(0)

/* called when ulp actually reuses the req and ownership is back to us */
#define returned_reuse_request(_ch, _req) do {\
	req_reused_bb_lru_del(_ch, _req);\
} while (0)

#define __del_reuse_request(req, disk) do {\
	req_reused_bb_state_switch(req, REUSED_BB_USED, REUSED_BB_TRY_END);\
	req_reused_bb_check(req, 1);\
	(req)->reused_bb = 0;\
	(req)->reused_bb_last_del_tag = 0xAA;\
	nvmeib_data_reuse_buf_zero(get_rcookie_ptr((req)->bcmd));\
	req_reused_bb_state_switch(req, REUSED_BB_TRY_END, REUSED_BB_UNUSED);\
	nvmeibc_disk_dec_reuse((disk));\
} while (0)

/* called by llp comp-ctx
   if bb was reused (after ec-jour-write for ec-data-write):
   1) mark it as unused before adding its parent req to available-(ch)-pool.
   2) destroy ulp's cookie so it won't rerurn it
 */
#define del_reuse_request(req, disk) ({	\
	bool ret = false;\
	if ((req)->reused_bb == 1) { 				\
		__del_reuse_request(req, disk);\
		ret = true;\
	}\
	ret;\
})

/* called by ulp to return bb to our ownership, cookie was already destroyed */
#define del_reuse_request_no_rcookie(ch, req) do {\
	req_reused_bb_state_switch(req, REUSED_BB_USED, REUSED_BB_TRY_END);\
	req_reused_bb_check(req, 1);\
	(req)->reused_bb = 0;\
	(req)->reused_bb_last_del_tag = 0xCC;\
	req_reused_bb_lru_del((ch), req);\
	req_reused_bb_state_switch(req, REUSED_BB_TRY_END, REUSED_BB_UNUSED);\
	nvmeibc_disk_dec_reuse((ch)->disk);\
} while (0)

int nvmeibc_ib_net_map_gen_data(struct nvmeibc_ib_net *net,
	struct nvmeibc_volume_request *req);
void nvmeibc_ib_net_unmap_gen_data(struct nvmeibc_ib_net *net,
								  struct nvmeibc_volume_request *req);

/* src buffer for piggyback write to jmdc on journal-write-op */
struct nvmeibc_disk_io_command;
struct nvmeibc_jmdc_pb_rsrc;
int nvmeibc_ib_net_jmdc_pb_map(struct nvmeib_dev *nv,
							   struct nvmeibc_jmdc_pb_rsrc *pb_rsrc,
							   binje_t binje);
void nvmeibc_ib_net_jmdc_pb_unmap(struct nvmeib_dev *nv,
								  struct nvmeibc_jmdc_pb_rsrc *pb_rsrc);
int nvmeibc_ib_net_jmdc_pb_fill_wrs(struct nvmeibc_ib_net *net,
								struct nvmeibc_disk_io_command *bcmd,
								struct nvmeib_remote_access_info *jmdc_rai,
								struct nvmeibc_jmdc_pb_rsrc *pb_rsrc,
								struct nvmeib_send_wr *wr_jmdc);

extern const struct vex_ops io_req_ops[];

/* This may false-alarm in case of cmd was explicitly completed by srv
   with error and net just "started" dying */
#define nvmeibc_in_net_warn_on_remote_cmd_wip(_net_, _comp_code_) 		\
do {																	\
	WARN_ONCE(_comp_code_ && atomic_read(&_net_->dying),				\
			  "net %p, %s: Unexpected, comp with err %d to ULP while "	\
			  "net is dying (state=%d), remote cmd might be wip\n",		\
			  _net_, _net_->ioch ? _net_->ioch->name : "<UNKNOWN>",		\
			  _comp_code_, nvmeib_get_state_guard(&_net_->state));		\
} while (0)

/* TBD GOODPATH_CORE:
 * -V- compress into bitfields (see @XXX) and format the dump via dictionary.json
 * -Defer for now, maybe totally abort- drop nvmeibc_block_bcmd_o_dbg_id() to avoid adding func call dbg vs. normal,
   copy dbg-id to bcmd...
 * add print to other disk access: Gen, Lock, Serjio
 * add print to control msgs [?]
 * -V- diff debug-level per op (write->NI, other->NT) [?]
 * add info to print:
   - piggyback (nvmeibc_indirect_piggyback_op)
   - disk-ver, ch-ver ...
   - srv: req->use_hw_blocks and req->use_sg
*/
#define NVMEIB_LOG_GOODPATH_CORE_POST(_name, _disk, _bcmd, _ch, _rsc_id, _lba, \
                                      _len, _op, _reuse_bb)                    \
	({                                                                         \
		if (nvmeib_block_io_op_is_read(_bcmd->reqs[0].op))                     \
			NVMEIB_LOG_GOODPATH_CORE_POST_LVL(                                 \
			    _DBG, NVMEIB_CONCAT2(_name, _read), _disk, _bcmd, _ch, _rsc_id,  \
			    _lba, _len, _op, _reuse_bb);                                   \
		else                                                                   \
			NVMEIB_LOG_GOODPATH_CORE_POST_LVL(_T, _name, _disk, _bcmd, _ch,    \
			                                  _rsc_id, _lba, _len, _op,        \
			                                  _reuse_bb);                      \
	})

#define NVMEIB_LOG_GOODPATH_CORE_POST_LVL(_level, _name, _disk, _bcmd, _ch,    \
                                          _rsc_id, _lba, _len, _op, _reuse_bb) \
	NVMEIB_LOG_GOODPATH(                                                       \
	    "{@O_DBG_ID} @GOODPATH_CORE_POST", _level, goodpath_nvmeibc_transport, \
	    _name, nvmeibc_block_bcmd_o_dbg_id(_bcmd), (u64)_disk, (u64)_bcmd,     \
	    _bcmd->reqs[0].op, get_rcookie_ptr(_bcmd)->action, (u64)_ch,       \
	    _rsc_id, _lba, _len, _op, _reuse_bb)


#if defined(NVMEIBC_READ_POISON_BB) && (NVMEIBC_READ_POISON_BB==1)
int nvmeibc_ib_net_poison_map(struct nvmeibc_ib_net *net);
void nvmeibc_ib_net_poison_unmap(struct nvmeibc_ib_net *net);
void nvmeibc_ib_net_poison_fill_wr(struct nvmeib_send_wr *wr, u64 id,
								   struct nvmeibc_ib_net *net,
								   struct nvmeibc_volume_request *req,
								   u64 raddr, u32 rkey);
void nvmeibc_ib_net_poison_verify(struct nvmeibc_ib_net *net,
								  struct nvmeibc_volume_request *req,
								  int *comp_code);
#else
#define nvmeibc_ib_net_poison_map(_net) (0)
#define nvmeibc_ib_net_poison_unmap(_net)
#define nvmeibc_ib_net_poison_fill_wr(_wr, _id, _net, _req, _raddr, _rkey)
#define nvmeibc_ib_net_poison_verify(_net, _req, _comp_code)
#endif


int nveibc_ib_net_defer_recv_interrupts_external(struct nvmeibc_ib_net *net);

int nvmeibc_ib_net_poll_cqs(struct nvmeibc_ib_net *net, bool notify);

#endif /* NVMEIBC_IB_NET_H */
