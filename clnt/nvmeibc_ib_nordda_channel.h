#ifndef NVMEIBC_IB_NORDDA_CHANNEL_H
#define NVMEIBC_IB_NORDDA_CHANNEL_H

#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibc_ib_net_nordda.h"
#include "nvmeibc_block.h"
#include "nvmeib_wd.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibc_types.h"
#include "nvmeibc_nr_lat_meas.h"

extern unsigned nr_max_used_reqs_per_channel;

enum {
	NVMEIBC_CHANNEL_MAX_MAIN_NORDDA_SG = NVMEIB_DEF_SG_PER_WQE,
	NVMEIBC_NR_CH_N_WR_CTRL = 1,
	NVMEIBC_NR_CH_RDMA_WRITE_JMDC_PB = 1,
	NVMEIBC_NR_CH_N_WR_IO_KA = 1,
	NVMEIBC_NR_CH_N_WR_FR = 2, /* Local Inv and Reg */
};

struct nvmeibc_volume_req_info {
	struct nvmeibc_volume_request req;
	struct nvmeibc_ib_nordda_channel *nrch;
	u64 raddr;
	int rn_pages;
	u32 rkey;
	struct nvmeibc_stats send;
	/* we must have more than 2,
	   8:  7 data wrs + 1 client-io-req wr
	   1:  metadata wr */
	struct nvmeib_send_wr wr[8 + 1];
	/* watchdog */
	struct wd_info_common wdc;
#if 0
	/* release counter - req is free iff counter is 0 */
	union {
		struct {
			int s_release_counter;
			int r_release_counter;
		};
		u64 counters;
	};
#endif
	int release_counter;
	/* piggyback read lock value */
	u64 pb_read_lock;
	u64 pb_read_lock_ioaddr;
	u32 pb_read_lock_rkey;
	int idx;
	//int dma_len;
	/* channel caching */
	u64 send_counter;
	u64 send_comp_counter;
	bool reused_bb_wait_send_comp;
	u64 reused_bb_wait_send_comp_start_cnt;
	u64 reused_bb_wait_send_comp_finish_cnt;

	/* we use the version when we reuse request. if the receive completion
	   arrives before the send completion and we wish to reuse the buffer
	   we reset the release counter and give the buffer to the block.  now in
	   the rare scenario where the block manages to use the buffer again before
	   the previous send_completion arrives then we will have a situation that
	   the previous send_completion when arrives will decrement the new release
	   counter and this may lead to corruption.  so to prevent it we add a
	   version to the request and the send_completion will check the version
	   and only iff the version in the returned wr_id matches to the version in
	   the request we will allow the decrement of the release counter...
	*/
	volatile u16 version;
	
	/* JMDC Piggy-back */
	struct nvmeibc_jmdc_pb_rsrc jmdc_pb;
	struct ib_sge md_sg;

	/* Debug: On WD, check which comp didnt arrive */
	u64 n_send_comp;
	u64 n_recv_comp;
	
	// Allow WD event w/o locking channel spinlock
	spinlock_t lock;
	int locking_pid;
	bool wd_timeout_occurred; // WD timeout happened - Used to disconnect channel outside of lock
	int n_wd_events;

	int comp_code;
	
	ktime_t send_time;

	struct nvmeibc_nr_lat_meas_nrch_req_meas lat_meas;
	
	struct nvmeibc_ib_net_pcpu_call_data pcpu_pending_io_smp_call;
	
	struct reuse_orig_t {
		union {
			struct ib_pool_fmr **fmr_list;
			struct nvmeib_fr_desc **fr_list;
		};
		short nmdesc;
		short sgcount;
	} reuse_orig;

	bool recv_comp_arrived;
};

static inline void nvmeibc_ib_nordda_channel_req_start_wd(
	struct nvmeibc_volume_req_info *req)
{
	req->n_wd_events = 0;
	nvmeib_wd_start_wdc(&req->wdc);
}

static inline void nvmeibc_ib_nordda_channel_req_stop_wd(
	struct nvmeibc_volume_req_info *req)
{
	nvmeib_wd_stop_wdc(&req->wdc);
}

/* clnt's no-RDDA channel */
struct nvmeibc_ib_nordda_channel {
	/* our base channel */
	struct nvmeibc_channel base;
	struct nvmeibc_ib_net_nordda net;
	spinlock_t guard;
	/* remove-work, that may run on c-nr-wq,
	   to wait for channel initialization
	   completion that runs on the admin-wq */
	struct completion init_comp;
	/* io reqs */
	struct nvmeibc_volume_req_info *reqs; /* [NVMEIB_MAX_NORDDA_IO_REQ] */
	DECLARE_BITMAP(req_in_use, NVMEIB_MAX_NORDDA_IO_REQ);
	struct list_head free_reqs;
	u32 n_used_reqs;
	u64 n_uses_ever;
	/* ad-hoc self-remove wq */
	struct workq_struct *release_wq;
	/* the local io nic we blongs to */
	struct nvmeibc_io_lnic *lionic;
	/* index of the channel in its lionic:rionic pair domain.
	   value range 0 to lionic->n_nr_qps-1, the max #of QPs
	   server allowed to have over this lionic:rionic pair
	   toward this disk via nordda */
	/* int index; ----- now in the base class ----- */
	/* link in disk's norddas list */
	struct list_head available_link;
	bool inuse;
	/* private 'shared' recvq */
	struct nvmeib_srq_info *priv_srq;
	/* receive queue (if not running with SRQ) */
	struct nvmeib_recvq *recv_q;
	atomic64_t last_watchdog_warning;	// used for throttling WD warnings
	atomic64_t last_received;

	/* recv completions WQ */
	struct nvmeib_q *rc_wq;

	/* ka remote access info */
	struct nvmeib_remote_access_info ka_rai;
	
	/* max op sizes */
	size_t max_io_sz;
	size_t max_gen_sz;

	/* value of nrch->inuse captured by disk_nr_iopaths_init() */
	bool iopaths_inuse;

	/* Priority of rionic */
	union nic_priority priority;
	/* latency measurements */
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data;

	unsigned int cpu;

	/* unique id in the scope of this disk
	   next val from disk->info->pcpu_nrchs_cnt */
	u64 pcpu_nrch_uid;
	
	struct work_struct pcpu_connect_work;
	struct completion pcpu_connect_comp;
	int connect_rv;
	
	/* Wait for both Send and Recv Completion
	 * before unmapping data/md and calling block callback.
	 * Needed for IOMMU enabled otherwise the occasional 
	 * LOC_PROT_ERR (due to a dropped ACK triggering data retransmission after Recv Completion) can trigger a PCIe Error.
	 */
	bool wait_release_zero_before_cb;
};

#define pcpu_nrch_cpu_set(__ch, __cpu, __ll)	nvmeibc_channel_pcpu_ch_set_cpu(&__ch->base, __cpu, __ll)
#define pcpu_nrch_cpu_clear(__ch)		nvmeibc_channel_pcpu_ch_clear_cpu(&__ch->base)
#define pcpu_nrch_cpu_get(__ch) 		nvmeibc_channel_pcpu_ch_get_cpu(&__ch->base)
#define is_pcpu_nrch(__ch)			nvmeibc_channel_is_pcpu_ch(&__ch->base)
#define is_ll_pcpu_nrch(__ch)			nvmeibc_channel_is_ll_pcpu_ch(&__ch->base)

#define is_coremask_nrch(__ch)				nvmeibc_channel_is_coremask_ch(&__ch->base)
#define get_coremask_nrch_cpu(__ch)			nvmeibc_channel_get_coremask_ch_cpu(&__ch->base)
#define get_coremask_nrch_cookie(__ch)			nvmeibc_channel_get_coremask_ch_cookie(&__ch->base)
#define set_coremask_nrch_cookie(__ch, __cookie)	nvmeibc_channel_set_coremask_ch_cookie(&__ch->base, __cookie)

#define nrch_guard_spin_lock_irqsave(nrch, flags) do {\
	if (is_ll_pcpu_nrch(nrch)) {\
		BUG_ON(get_cpu() != pcpu_nrch_cpu_get(nrch));\
		local_irq_save(flags);\
	} else {\
		spin_lock_irqsave(&nrch->guard, flags);\
	}\
} while(0)

#define nrch_guard_spin_unlock_irqrestore(nrch, flags) do {\
	if (is_ll_pcpu_nrch(nrch)) {\
		BUG_ON(smp_processor_id() != pcpu_nrch_cpu_get(nrch));\
		local_irq_restore(flags);\
		put_cpu();\
	} else {\
		spin_unlock_irqrestore(&nrch->guard, flags);\
	}\
} while(0)

#define ri_spin_lock_irqsave(ri, flags) do { \
	if (is_ll_pcpu_nrch(ri->nrch)) {\
		BUG_ON(get_cpu() != pcpu_nrch_cpu_get(ri->nrch));\
		local_irq_save(flags);\
	} else { \
		spin_lock_irqsave(&ri->lock, flags); \
	}\
	ri->locking_pid = current->pid; \
} while(0)

#define ri_spin_lock(ri) do { \
	if (is_ll_pcpu_nrch(ri->nrch))\
		BUG_ON(get_cpu() != pcpu_nrch_cpu_get(ri->nrch));\
	else \
		spin_lock(&ri->lock);\
	ri->locking_pid = current->pid;\
} while(0)

#define ri_spin_unlock_irqrestore(ri, flags) do { \
	if (is_ll_pcpu_nrch(ri->nrch)) {\
		BUG_ON(smp_processor_id() != pcpu_nrch_cpu_get(ri->nrch));\
		ri->locking_pid = -1;\
		local_irq_restore(flags);\
		put_cpu();\
	} else { \
		ri->locking_pid = -1;\
		spin_unlock_irqrestore(&ri->lock, flags);\
	}\
} while(0)

#define ri_spin_unlock(ri) do { \
	if (is_ll_pcpu_nrch(ri->nrch)) {\
		ri->locking_pid = -1;\
		BUG_ON(smp_processor_id() != pcpu_nrch_cpu_get(ri->nrch));\
		put_cpu();\
	} else { \
		ri->locking_pid = -1;\
		spin_unlock(&ri->lock);\
	}\
} while(0)

static inline bool ri_already_locked(struct nvmeibc_volume_req_info *ri)
{
	if (is_ll_pcpu_nrch(ri->nrch)) {
		if (preemptible())
			return false;
		BUG_ON(smp_processor_id() != pcpu_nrch_cpu_get(ri->nrch));
		return ri->locking_pid == current->pid;
	}
	return irqs_disabled() && ri->locking_pid == current->pid;
}

#define REUSE_SG_FR_STORE(_req) \
	do { \
		extern bool nvmeibc_nr_store_fr; \
		if (nvmeibc_nr_store_fr && (_req)->req.sgcount) { \
			BUG_ON((_req)->reuse_orig.sgcount || (_req)->reuse_orig.nmdesc || (_req)->reuse_orig.fr_list); \
			(_req)->reuse_orig.sgcount = (_req)->req.sgcount; \
			(_req)->req.sgcount = 0; \
			if ((_req)->req.nmdesc) { \
				(_req)->reuse_orig.fr_list = (_req)->req.fr_list; \
				(_req)->reuse_orig.nmdesc = (_req)->req.nmdesc; \
				(_req)->req.fr_list = NULL; \
				(_req)->req.nmdesc = 0; \
			} \
		} \
	} while (0)

#define REUSE_FR_RESTORE(_req) \
	do { \
		if ((_req)->reuse_orig.nmdesc) { \
			BUG_ON((_req)->req.fr_list || (_req)->req.nmdesc); \
			(_req)->req.fr_list = (_req)->reuse_orig.fr_list; \
			(_req)->req.nmdesc = (_req)->reuse_orig.nmdesc; \
			(_req)->reuse_orig.fr_list = NULL; \
			(_req)->reuse_orig.nmdesc = 0; \
		} \
	} while (0)

#define REUSE_SG_RESTORE(_req) \
	do { \
		BUG_ON((_req)->req.sgcount); \
		(_req)->req.sgcount = (_req)->reuse_orig.sgcount; \
		(_req)->reuse_orig.sgcount = 0; \
	} while (0)

static inline struct nvmeibc_volume_req_info *r_to_sri(
	struct nvmeibc_volume_request *req)
{
	return container_of(req, struct nvmeibc_volume_req_info, req);
}

static inline struct nvmeibc_ib_nordda_channel *c_to_inrc(
	struct nvmeibc_channel *ch)
{
	return container_of(ch, struct nvmeibc_ib_nordda_channel, base);
}

static inline struct nvmeibc_ib_nordda_channel *innr_to_inrc(
	struct nvmeibc_ib_net_nordda *net)
{
	return container_of(net, struct nvmeibc_ib_nordda_channel, net);
}

static inline struct nvmeibc_ib_nordda_channel *in_to_inrc(
	struct nvmeibc_ib_net *net)
{
	return innr_to_inrc(in_to_inrn(net));
}

struct nvmeibc_cinst_params_core;
struct nvmeibc_ib_nordda_channel *nvmeibc_ib_nordda_channel_create(
	const struct nvmeibc_cinst_params_core *p, struct nvmeibc_io_lnic *lionic,
	int qpn, int lionic_index, int rionic_index);
int nvmeibc_ib_nordda_channel_init_reqs(struct nvmeibc_ib_nordda_channel *ch,
	struct volume_server_config_alloc_nr_net_rsp *a_nr_net_rsp);
int nvmeibc_ib_nordda_channel_connect(struct nvmeibc_ib_nordda_channel *ch);
bool nvmeibc_ib_nordda_channel_try_disconnect(
	struct nvmeibc_ib_nordda_channel *ch);
void nvmeibc_ib_nordda_channel_clear_rq(struct nvmeibc_ib_nordda_channel *ch,
	bool cd);
void nvmeibc_ib_nordda_channel_free(struct nvmeibc_ib_nordda_channel *ch);
struct nvmeib_iu* nvmeibc_ib_nordda_channel_get_rx_iu(struct nvmeibc_ib_nordda_channel *ch, int index);
int nvmeibc_ib_nordda_channel_put_rx_iu(struct nvmeibc_ib_nordda_channel *ch, struct nvmeib_iu *iu);
void *nvmeibc_ib_nordda_channel_get_io_context(
	struct nvmeibc_ib_nordda_channel *ch);
void nvmeibc_ib_nordda_channel_init_stats(struct nvmeibc_ib_nordda_channel *ch);
void nvmeibc_ib_nordda_channel_print_stats(struct nvmeibc_ib_nordda_channel *ch,
	const char *str);

void nvmeibc_ib_nordda_channel_use(struct nvmeibc_ib_nordda_channel *ch);
void nvmeibc_ib_nordda_channel_end_use(struct nvmeibc_ib_nordda_channel *ch);
bool nvmeibc_ib_nordda_channel_is_used(struct nvmeibc_ib_nordda_channel *ch);

bool nvmeibc_ib_nordda_channel_check_reused(
	struct nvmeibc_ib_nordda_channel *ch, void *context);
void nvmeibc_ib_nordda_channel_reused_context(struct nvmeibc_ib_nordda_channel *ch,
	struct nvmeib_data_reuse_buf_params *p, void **context);
bool nvmeibc_ib_nordda_channel_alive(struct nvmeibc_ib_nordda_channel *ch);

int nvmeibc_ib_nordda_channel_poll_cqs(struct nvmeibc_ib_nordda_channel *ch, bool notify);

#endif
