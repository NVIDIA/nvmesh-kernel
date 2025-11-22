#define  S_NORDDA_C

#include "common/kr_incs.h"

#if !defined(BLKDEV_SIMULATOR)
/* Strictly non simulator includes */
#include "nvmeibs_nordda.h"
#include "nvmeibs_toma.h"
#include "nvmeib.h"
#include "nvmeibs_defs.h"
#include "nvmeib_utils.h"
#include "nvmeib_srq.h"
#include "nvmeibs_trace.h"
#include "nvmeibc_block.h"
#include "vex/nvmeibs_vex.h"
#include "nvmeibs_nordda_sim_shared.h"
#include "nvmeibs_main_gen_cmds.h"
#include "nvmeibs_async_cookies.h"
#include "nvmeibs_memmgr_metrics.h"
/* All the code in this file does not compile in simulator,
   except some explicitly marked chunks that are shared. */

#define ___NFIN _ND(_name_, "--> @NAME\n", nrch->name)
#define ___NFOUT _ND(_name_, "<-- @NAME\n", nrch->name)
#define __NFIN
#define __NFOUT

#define USE_PRIV_SRQ 	0
#define PRIV_SRQ_SIZE 	NVMEIB_NORDDA_SRQ_MAX_SIZE

/* Instead of using the global DMA key for MD page of BB, use BB MR Key to provide overflow protection */
#define USE_BB_MAP_MR_FOR_MD	1

#define NVMEIBS_NORDDA_ASSUME_DATA_MR(_port_hw) ({\
	(void)_port_hw;\
	true;\
})

#define NVMEIBS_CLIENT_NRCH_DEFER_VMAP(_cl) ((_cl)->di->defer_vmap)

#if NVMEIBS_NR_LAT_MEAS
#define NRCH_PCPU_STAT_INC(nrch, field) \
do {	\
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;\
	unsigned long flags;\
	local_irq_save(flags);\
	stats_per_cpu = this_cpu_ptr(nrch->lat_meas.stats_per_cpu);	\
	stats_per_cpu->field++;	\
	local_irq_restore(flags);\
} while(0)

#define NRCH_PCPU_STAT_ADD(nrch, field, val) \
do {	\
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;\
	unsigned long flags;\
	local_irq_save(flags);\
	stats_per_cpu = this_cpu_ptr(nrch->lat_meas.stats_per_cpu);	\
	stats_per_cpu->field += val;	\
	local_irq_restore(flags);\
} while(0)

#define NRCH_PCPU_STAT_SET(nrch, field, val) \
do {	\
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;\
	unsigned long flags;\
	local_irq_save(flags);\
	stats_per_cpu = this_cpu_ptr(nrch->lat_meas.stats_per_cpu);	\
	stats_per_cpu->field = val;	\
	local_irq_restore(flags);\
} while(0)

#define NRCH_PCPU_STAT_MIN_UPDATE(nrch, field, new_min) \
do {	\
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;\
	unsigned long flags;\
	local_irq_save(flags);\
	stats_per_cpu = this_cpu_ptr(nrch->lat_meas.stats_per_cpu);	\
	stats_per_cpu->field = stats_per_cpu->field == 0 ? new_min : min_t(typeof(stats_per_cpu->field), stats_per_cpu->field, new_min);	\
	local_irq_restore(flags);\
} while(0)

#define NRCH_PCPU_STAT_MAX_UPDATE(nrch, field, new_max) \
do {	\
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;\
	unsigned long flags;\
	local_irq_save(flags);\
	stats_per_cpu = this_cpu_ptr(nrch->lat_meas.stats_per_cpu);	\
	stats_per_cpu->field = stats_per_cpu->field == 0 ? new_max : max_t(typeof(stats_per_cpu->field), stats_per_cpu->field, new_max);	\
	local_irq_restore(flags);\
} while(0)

static void nvmeibs_nr_lat_meas_record_io_cmd_cb(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd) {
	u64 lat_us;
	/* Update stats */
	cmd->lat_meas.comp_time = nvmeib_public_ktime_get();
	lat_us = ktime_to_us(ktime_sub(cmd->lat_meas.comp_time, cmd->lat_meas.submit_time));
	NRCH_PCPU_STAT_INC(nrch, n_iops_comp);
	NRCH_PCPU_STAT_ADD(nrch, tot_lat_us, lat_us);
	NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_lat_us, lat_us);
	NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_lat_us, lat_us);
}

static void nvmeibs_nr_lat_meas_record_io_submit(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd)
{
	NRCH_PCPU_STAT_INC(nrch, n_iops_submit);
	cmd->lat_meas.submit_time = nvmeib_public_ktime_get();
}

static void nvmeibs_nr_lat_meas_record_send_rsp(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd)
{
	(void)nrch;
	cmd->lat_meas.post_send_time = nvmeib_public_ktime_get();
}

static void nvmeibs_nr_lat_meas_record_rx_md(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *recv_ioctx)
{
	if (recv_ioctx->rx_md.valid) {
		u64 recv_poll_lat_us = ktime_to_us(ktime_sub(recv_ioctx->rx_md.poll_time, recv_ioctx->rx_md.recv_time));
		u64 recv_submit_lat_us = ktime_to_us(ktime_sub(nvmeib_public_ktime_get(), recv_ioctx->rx_md.recv_time));
		NRCH_PCPU_STAT_ADD(nrch, tot_recv_poll_lat_us, recv_poll_lat_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_recv_poll_lat_us, recv_poll_lat_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_recv_poll_lat_us, recv_poll_lat_us);
		NRCH_PCPU_STAT_ADD(nrch, tot_recv_submit_lat_us, recv_submit_lat_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_recv_submit_lat_us, recv_submit_lat_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_recv_submit_lat_us, recv_submit_lat_us);
		NRCH_PCPU_STAT_INC(nrch, rx_cpu_cnt[recv_ioctx->rx_md.cpu]);
		NRCH_PCPU_STAT_INC(nrch, rx_queue_cnt[recv_ioctx->rx_md.queue]);
		NRCH_PCPU_STAT_SET(nrch, rx_skb_hash, recv_ioctx->rx_md.skb_hash);
	}
}

static void nvmeibs_nr_lat_meas_fill_rx_md(const struct ib_wc *recv_wc, struct nvmeib_iu *recv_ioctx)
{
	ktime_t start_recv_time; /* Not used for stats */
	if (!(recv_ioctx->rx_md.valid = nvmeib_rdma_siw_wc_get_rx_md(recv_wc,
		&start_recv_time,
		&recv_ioctx->rx_md.recv_time,
		&recv_ioctx->rx_md.poll_time,
		&recv_ioctx->rx_md.cpu,
		&recv_ioctx->rx_md.queue,
		&recv_ioctx->rx_md.skb_hash))) {
		/* Did not get MD from SIW MD, fill it as best we can */
		recv_ioctx->rx_md.recv_time = nvmeib_public_ktime_get();
		recv_ioctx->rx_md.poll_time = nvmeib_public_ktime_get();
		recv_ioctx->rx_md.cpu = smp_processor_id();
		recv_ioctx->rx_md.valid = true;
	}
}

static void nvmeibs_nr_lat_meas_on_post_recv(struct nvmeib_iu *recv_ioctx)
{
	memset(&recv_ioctx->rx_md, 0, sizeof_field(struct nvmeib_iu, rx_md));
}

static void nvmeibs_nr_lat_meas_record_send_comp(const struct ib_wc *wc_send, struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	ktime_t post_send_time;
	ktime_t sent_time;
	ktime_t ack_time;
	ktime_t send_comp_time = nvmeib_public_ktime_get();
	u64 send2comp_us;
	u16 tx_cpu;

	if (nvmeib_rdma_siw_wc_get_tx_md(wc_send, &post_send_time, &sent_time, &ack_time, &tx_cpu)) {
		u64 send_time_us = ktime_to_us(ktime_sub(sent_time, post_send_time));
		u64 ack_time_us = ktime_to_us(ktime_sub(ack_time, sent_time));

		send2comp_us = ktime_to_us(ktime_sub(send_comp_time, post_send_time));

		NRCH_PCPU_STAT_ADD(nrch, tot_send_time_us, send_time_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_send_time_us, send_time_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_send_time_us, send_time_us);
		NRCH_PCPU_STAT_ADD(nrch, tot_ack_time_us, ack_time_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_ack_time_us, ack_time_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_ack_time_us, ack_time_us);
		NRCH_PCPU_STAT_ADD(nrch, tot_send2comp_time_us, send2comp_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_send2comp_time_us, send2comp_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_send2comp_time_us, send2comp_us);
		NRCH_PCPU_STAT_INC(nrch, tx_cpu_cnt[tx_cpu]);
	} else {
		send2comp_us = ktime_to_us(ktime_sub(send_comp_time, cmd->lat_meas.post_send_time));

		NRCH_PCPU_STAT_ADD(nrch, tot_send2comp_time_us, send2comp_us);
		NRCH_PCPU_STAT_MIN_UPDATE(nrch, min_send2comp_time_us, send2comp_us);
		NRCH_PCPU_STAT_MAX_UPDATE(nrch, max_send2comp_time_us, send2comp_us);
		tx_cpu = smp_processor_id();
		NRCH_PCPU_STAT_INC(nrch, tx_cpu_cnt[tx_cpu]);
	}
}

#if ENABLE_SIW
#include "../softiwarp/kernel/siw.h"
#endif

static ssize_t fill_nrch_stats(void *arg, char *buf, size_t len)
{
	#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	struct nvmeibs_nr_channel *nrch = arg;
	struct nvmeibs_nrch_stats_per_cpu *stats_per_cpu;
	struct nvmeibs_nrch_stats_per_cpu *stats_total = NULL;
	ssize_t count = 0;
	int cpu, i;
	bool is_siw = P2NV(NR2P(nrch))->dev_type == DT_siw;

	if (!(stats_total = kzalloc(sizeof(*stats_total), GFP_ATOMIC))) {
		BUF_ADD("Out of Memory\n");
		goto out;
	}

	for_each_possible_cpu(cpu) {
		stats_per_cpu = per_cpu_ptr(nrch->lat_meas.stats_per_cpu, cpu);
		stats_total->n_iops_submit += stats_per_cpu->n_iops_submit;
		stats_total->n_iops_comp += stats_per_cpu->n_iops_comp;
		stats_total->n_iops_err += stats_per_cpu->n_iops_err;

		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			stats_total->tx_cpu_cnt[i] += stats_per_cpu->tx_cpu_cnt[i];
			stats_total->rx_cpu_cnt[i] += stats_per_cpu->rx_cpu_cnt[i];
			stats_total->rx_queue_cnt[i] += stats_per_cpu->rx_queue_cnt[i];
		}

		if (stats_per_cpu->rx_skb_hash)
			stats_total->rx_skb_hash = stats_per_cpu->rx_skb_hash;

		if (stats_per_cpu->n_iops_submit) {
			if (is_siw) {
				stats_total->tot_recv_poll_lat_us += stats_per_cpu->tot_recv_poll_lat_us;
				stats_total->min_recv_poll_lat_us = min(stats_total->min_recv_poll_lat_us, stats_per_cpu->min_recv_poll_lat_us);
				stats_total->max_recv_poll_lat_us = max(stats_total->max_recv_poll_lat_us, stats_per_cpu->max_recv_poll_lat_us);
				stats_total->tot_recv_submit_lat_us += stats_per_cpu->tot_recv_submit_lat_us;
				stats_total->min_recv_submit_lat_us = min(stats_total->min_recv_poll_lat_us, stats_per_cpu->min_recv_submit_lat_us);
				stats_total->max_recv_submit_lat_us = max(stats_total->max_recv_poll_lat_us, stats_per_cpu->max_recv_submit_lat_us);
			}
		}
		if (stats_per_cpu->n_iops_comp) {
			stats_total->tot_lat_us += stats_per_cpu->tot_lat_us;
			stats_total->min_lat_us = min(stats_total->min_lat_us, stats_per_cpu->min_lat_us);
			stats_total->max_lat_us = max(stats_total->max_lat_us, stats_per_cpu->max_lat_us);
			if (is_siw) {
				stats_total->tot_send_time_us += stats_per_cpu->tot_send_time_us;
				stats_total->min_send_time_us = min(stats_total->min_send_time_us, stats_per_cpu->min_send_time_us);
				stats_total->max_send_time_us = max(stats_total->max_send_time_us, stats_per_cpu->max_send_time_us);
				stats_total->tot_ack_time_us += stats_per_cpu->tot_ack_time_us;
				stats_total->min_ack_time_us = min(stats_total->min_ack_time_us, stats_per_cpu->min_ack_time_us);
				stats_total->max_ack_time_us = max(stats_total->max_ack_time_us, stats_per_cpu->max_ack_time_us);
			}
			stats_total->tot_send2comp_time_us += stats_per_cpu->tot_send2comp_time_us;
			stats_total->min_send2comp_time_us = min(stats_total->min_send2comp_time_us, stats_per_cpu->min_send2comp_time_us);
			stats_total->max_send2comp_time_us = max(stats_total->max_send2comp_time_us, stats_per_cpu->max_send2comp_time_us);
		}
	}
	BUF_ADD("NRCH: %s (%px) INDEX: %d\n", nrch->name, nrch, nrch->net->params.ch_index);
	BUF_ADD("NET: %px\n", nrch->net);
	BUF_ADD("QP: %d (%px)\n", nrch->net->qp->qp_num, nrch->net->qp);

#if ENABLE_SIW
	if (P2NV(NR2P(nrch))->dev_type == DT_siw) {
		struct siw_qp *siw_qp = container_of(nrch->net->qp, struct siw_qp, ofa_qp);
		struct socket *socket = siw_qp->attrs.llp_stream_handle;
		struct sock *sk = socket->sk;
		int scq_vector = siw_qp->scq->comp_vector;
		int rcq_vector = siw_qp->rcq->comp_vector;
		BUF_ADD("%pI4n:%u -> %pI4n:%u (%08x)\n",
			&sk->sk_daddr, sk->sk_dport, &sk->sk_rcv_saddr, sk->sk_num, stats_total->rx_skb_hash);
#	if KS_HAS_SO_INCOMING_CPU
		BUF_ADD("socket: %px sk: %px incoming_cpu: %d napi_id: %d scq_vector: %d rcq_vector: %d\n",
			socket, sk, READ_ONCE(sk->sk_incoming_cpu), READ_ONCE(sk->sk_napi_id), scq_vector, rcq_vector);
#	else
		BUF_ADD("socket: %px sk: %px napi_id: %d scq_vector: %d rcq_vector: %d\n",
			socket, sk, READ_ONCE(sk->sk_napi_id), scq_vector, rcq_vector);
#	endif
#	if SIW_SRQ_RQE_TRACK
		if (nrch->net->qp->srq) {
			struct siw_srq *siw_srq = container_of(nrch->net->qp->srq, struct siw_srq, ofa_srq);
			BUF_ADD("SIW SRQ %px: n_q/max_q: %lu/%lu n_recv: %lu n_cq: %lu n_ulp: %lu\n",
				siw_srq, siw_srq->rqe_track.n_q, siw_srq->rqe_track.max_q,
				siw_srq->rqe_track.n_recv, siw_srq->rqe_track.n_cq,
				siw_srq->rqe_track.n_ulp);
		}
#	endif
	}
	#endif

	BUF_ADD("n_iops_submit=%llu\n", stats_total->n_iops_submit);
	BUF_ADD("n_iops_comp=%llu\n", stats_total->n_iops_comp);
	BUF_ADD("n_iops_err=%llu\n", stats_total->n_iops_err);
	if (stats_total->n_iops_submit) {
		if (is_siw) {
			BUF_ADD("recv_poll_us (min/avg/max) = (%u/%llu/%u)\n",
				stats_total->min_recv_poll_lat_us, stats_total->tot_recv_poll_lat_us / stats_total->n_iops_submit, stats_total->max_recv_poll_lat_us);
			BUF_ADD("recv_submit_us (min/avg/max) = (%u/%llu/%u)\n",
				stats_total->min_recv_submit_lat_us, stats_total->tot_recv_submit_lat_us / stats_total->n_iops_submit, stats_total->max_recv_submit_lat_us);
		}
	}
	if (stats_total->n_iops_comp) {
		BUF_ADD("latency_us (min/avg/max) = (%u/%llu/%u)\n",
			stats_total->min_lat_us, stats_total->tot_lat_us / stats_total->n_iops_comp, stats_total->max_lat_us);
		if (is_siw) {
			BUF_ADD("send_time_us (min/avg/max) = (%u/%llu/%u)\n",
				stats_total->min_send_time_us, stats_total->tot_send_time_us / stats_total->n_iops_comp, stats_total->max_send_time_us);
			BUF_ADD("ack_time_us (min/avg/max) = (%u/%llu/%u)\n",
				stats_total->min_ack_time_us, stats_total->tot_ack_time_us / stats_total->n_iops_comp, stats_total->max_ack_time_us);
		}
		BUF_ADD("send2comp_time_us (min/avg/max) = (%u/%llu/%u)\n",
			stats_total->min_send2comp_time_us, stats_total->tot_send2comp_time_us / stats_total->n_iops_comp, stats_total->max_send2comp_time_us);
	}
	BUF_ADD("tx_cpu_cnt -");
	for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
		if (cpu_online(i) && stats_total->tx_cpu_cnt[i])
			BUF_ADD("\t[%d] - %llu", i, stats_total->tx_cpu_cnt[i]);
	}
	BUF_ADD("\n");
	BUF_ADD("rx_cpu_cnt -");
	for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
		if (cpu_online(i) && stats_total->rx_cpu_cnt[i])
			BUF_ADD("\t[%d] - %llu", i, stats_total->rx_cpu_cnt[i]);
	}
	BUF_ADD("\n");
	if (is_siw) {
		BUF_ADD("rx_queue_cnt -");
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (stats_total->rx_queue_cnt[i])
				BUF_ADD("\t[%d] - %llu", i, stats_total->rx_queue_cnt[i]);
		}
		BUF_ADD("\n");
		BUF_ADD("rx_skb_hash - %08x\n", stats_total->rx_skb_hash);
	}

	kfree(stats_total);
	out:
	return count;
	#undef BUF_ADD
}

static int nvmeibs_nr_lat_meas_nrch_alloc(struct nvmeibs_client *cl, struct nvmeibs_nr_channel *nrch)
{
	int rv;
	/* Create proc and stats stuff */
	if (!(nrch->lat_meas.stats_per_cpu = nvmeib_public_alloc_percpu_cacheline(struct nvmeibs_nrch_stats_per_cpu))) {
		_NE(err_connect_nrch_ch_work_alloc_stats, "Failed to create stats per cpu for nrch @NRCH_NAME", nrch->name);
		rv = -ENOMEM;
		goto out;
	}

	if (IS_ERR_OR_NULL(nrch->lat_meas.parent_proc_dir = get_disk_proc_dir(cl->di))) {
		nrch->lat_meas.parent_proc_dir = nvmeibs_proc_dir;
	}

	if (!(nrch->lat_meas.proc_dir = proc_mkdir(nrch->name, nrch->lat_meas.parent_proc_dir))) {
		_NE(err_connect_nrch_ch_work_proc_dir, "Failed to create /proc dir for nrch @NRCH_NAME", nrch->name);
		rv = -ENOMEM;
		goto out;
	}

	if (!(nrch->lat_meas.stats_proc_file = nvmeib_public_proc_create("stats", nrch->lat_meas.proc_dir, fill_nrch_stats,
		NULL, nrch))) {
		_NE(err_connect_nrch_ch_work_proc_stats, "Failed to create stats proc for nrch @NRCH_NAME", nrch->name);
		rv = -ENOMEM;
		goto out;
	}
	rv = 0;
out:
	return rv;
}

static void nvmeibs_nr_lat_meas_nrch_free(struct nvmeibs_nr_channel *nrch)
{
	if (nrch->lat_meas.stats_proc_file) {
		nvmeib_public_proc_remove(nrch->lat_meas.stats_proc_file);
		nrch->lat_meas.stats_proc_file = NULL;
	}
	if (nrch->lat_meas.proc_dir) {
		remove_proc_entry(nrch->name, nrch->lat_meas.parent_proc_dir);
		nrch->lat_meas.proc_dir = NULL;
	}
	nvmeib_public_free_percpu(nrch->lat_meas.stats_per_cpu);
	nrch->lat_meas.stats_per_cpu = NULL;
}

static int nvmeibs_nr_lat_meas_alloc_siw_md(struct nvmeibs_nr_channel *nrch)
{
	int rv;
	/* Allocate MD for SIW WCs */
	if (!(nrch->lat_meas.scq_wcs_siw_md = nvmeib_rdma_alloc_siw_wc_md(nrch->n_scq_wcs, nrch->scq_wcs, GFP_KERNEL)) ||
		!(nrch->lat_meas.rcq_wcs_siw_md = nvmeib_rdma_alloc_siw_wc_md(nrch->n_rcq_wcs, nrch->rcq_wcs, GFP_KERNEL))) {

		_NE(err_nordda_connect_nordda_channel_work_wcs_siw_md_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}
	rv = 0;
out:
	return rv;
}

static void nvmeibs_nr_lat_meas_free_siw_md(struct nvmeibs_nr_channel *nrch)
{
	/* Free SIW WC MD (if allocated) */
	nvmeib_rdma_free_siw_wc_md(nrch->lat_meas.scq_wcs_siw_md, 0, NULL);
	nrch->lat_meas.scq_wcs_siw_md = NULL;
	nvmeib_rdma_free_siw_wc_md(nrch->lat_meas.rcq_wcs_siw_md, 0, NULL);
	nrch->lat_meas.rcq_wcs_siw_md = NULL;
}

#else
#define NRCH_PCPU_STAT_INC(nrch, field)
#define NRCH_PCPU_STAT_ADD(nrch, field, val)
#define NRCH_PCPU_STAT_SET(nrch, field, val)
#define NRCH_PCPU_STAT_MIN_UPDATE(nrch, field, new_min)
#define NRCH_PCPU_STAT_MAX_UPDATE(nrch, field, new_max)

static void nvmeibs_nr_lat_meas_record_io_cmd_cb(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd)
{
	(void)nrch;
	(void)cmd;
}

static void nvmeibs_nr_lat_meas_record_io_submit(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd)
{
	(void)nrch;
	(void)cmd;
}

static void nvmeibs_nr_lat_meas_record_send_rsp(struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd)
{
	(void)nrch;
	(void)cmd;
}

static void nvmeibs_nr_lat_meas_record_rx_md(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *recv_ioctx)
{
	(void)nrch;
	(void)recv_ioctx;
}

static void nvmeibs_nr_lat_meas_fill_rx_md(const struct ib_wc *recv_wc, struct nvmeib_iu *recv_ioctx)
{
	(void)recv_wc;
	(void)recv_ioctx;
}

static void nvmeibs_nr_lat_meas_on_post_recv(struct nvmeib_iu *recv_ioctx)
{
	(void)recv_ioctx;
}

static void nvmeibs_nr_lat_meas_record_send_comp(const struct ib_wc *wc_send, struct nvmeibs_nr_cmd *cmd)
{
	(void)wc_send;
	(void)cmd;
}

static int nvmeibs_nr_lat_meas_nrch_alloc(struct nvmeibs_client *cl, struct nvmeibs_nr_channel *nrch)
{
	(void)nrch;
	return 0;
}

static void nvmeibs_nr_lat_meas_nrch_free(struct nvmeibs_nr_channel *nrch)
{
	(void)nrch;
}

static int nvmeibs_nr_lat_meas_alloc_siw_md(struct nvmeibs_nr_channel *nrch)
{
	(void)nrch;
	return 0;
}

static void nvmeibs_nr_lat_meas_free_siw_md(struct nvmeibs_nr_channel *nrch)
{
	(void)nrch;
}

#endif

static void nordda_send_comp_h(void *ctx, struct ib_wc *wcs);
static void nordda_recv_comp_h(void *ctx, struct ib_wc *wcs);

bool nvmeibs_nr_post_recv_on_send_comp = false;
module_param_named(nr_post_recv_on_send_comp, nvmeibs_nr_post_recv_on_send_comp, bool, 0444);
MODULE_PARM_DESC(nr_post_recv_on_send_comp, "In percpu CQ and SRQ mode, post recv buff on send comp of io-rsp");

bool nvmeibs_nr_skip_disk_access = false;
module_param_named(nr_skip_disk_access, nvmeibs_nr_skip_disk_access, bool, 0644);
MODULE_PARM_DESC(nr_skip_disk_access, "Unsafe debug mode: No-rdda skip disk access");

bool nvmeibs_nr_skip_rdma_write_back = false;
module_param_named(nr_skip_rdma_write_back, nvmeibs_nr_skip_rdma_write_back, bool, 0644);
MODULE_PARM_DESC(nr_skip_rdma_write_back, "Unsafe debug mode: No-rdda skip rdma write-back in read operation");

bool nvmeibs_nr_wq_set_cpu_affinity = false;
module_param_named(nr_wq_set_cpu_affinity, nvmeibs_nr_wq_set_cpu_affinity, bool, 0644);
MODULE_PARM_DESC(nr_wq_set_cpu_affinity, "Set CPU affinity of NR WQ (based on channel index)");

void nrch_send_rsp_and_post_recv(struct nvmeibs_nr_channel *nrch,
	u8 opcode, u64 tag, u16 version_tag, struct nvmeib_iu *send_ioctx, void *p, int len,
	int wr_opcode, u16 wr_version, struct nvmeib_iu *recv_ioctx);
struct nvmeib_iu *rxiu_pop(struct nvmeibs_nr_channel *nrch);
void rxiu_drain(struct nvmeibs_nr_channel *nrch);

enum nordda_alloc_timeouts {
	NR_ALLOC_TIMEOUT_SINGLE_VMAP_SLOW_WARN 	 = HZ/10,
	NR_ALLOC_TIMEOUT_VMAP_SLOW_RESCHED     	 = HZ*6,
	NR_ALLOC_TIMEOUT_CMD_BB_ALLOC_WARN    	 = HZ/5,
	NR_ALLOC_TIMEOUT_IO_CMD_BB_ALLOC_WARN     = HZ/5
};

NVMEIBS_MEMMGR_METRIC(s_nordda_srq_info, "component=target.nordda.srq_info");
NVMEIBS_MEMMGR_METRIC(s_nordda_bb_pages, "component=target.nordda.bb_pages"); /* includes bounce buffer and prpl page */

/* -------------------------------------------------------------------------- */
/* Receive                                                                    */
/* -------------------------------------------------------------------------- */
static void handle_io_cmd(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx);
static void handle_io_cmd_underway(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx);
static inline void io_cmd_defer(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx);

enum can_handle_res {
	/* net is connecting --> handled on rts (net goes live), or
	   no free send-iu   --> handled on next IB completion  */
	NORDDA_PENDING,

	/* managed to get send iu */
	NORDDA_NOW,

	/* managed to get send iu,
	   but completions burst  */
	NORDDA_DEFER,
};

int nvmeibs_nordda_add_work(struct nvmeibs_nr_channel *nrch,
	struct workqe_struct *work)
{
	return wq_add_work(nrch->wq, work) ? 0 : -1;
}

static inline bool new_cmd_underway(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *recv_ioctx)
{
	bool ret;

	NFIN;

	ret = atomic_inc_not_zero(&nrch->underway_cmds) > 0;

#ifdef NVMEIBS_NR_CATCH_CMD_ALREADY_UNDERWAY
	if (ret) {
		struct nvmeibs_client *cl = NR2C(nrch);
		struct volume_client_req *req = recv_ioctx->buf;
		u64 req_tag = be64_to_cpu(req->hdr.tag);
		u16 index = nordda_tag_decode_index(req_tag);
		u16 version = nordda_tag_decode_version(req_tag);
		u32 ch_version = nordda_tag_decode_ch_version(req_tag);
		unsigned long flags;

		BUG_ON(index >= cl->nrch_ioreq_num);
		spin_lock_irqsave(&nrch->spinlock, flags);
		if (test_and_set_bit(index, nrch->underway_cmds_bmp)) {
			_NE(error_s_nordda_new_cmd_underway_already,
				"nrch @NRCH_NAME (@NRCH), idx @INDEX, already underway. req version @VERSION req channel version @VERSION",
				nrch->name, nrch, index, version, ch_version);
			BUG_ON(1);
		}
		spin_unlock_irqrestore(&nrch->spinlock, flags);
	}
#else
	(void)recv_ioctx;
#endif

	NFOUT;
	return ret;
}

static inline void cmd_ended(struct nvmeibs_nr_channel *nrch, __be64 req_hdr_tag)
{
	NFIN;
#ifdef NVMEIBS_NR_CATCH_CMD_ALREADY_UNDERWAY
	do {
		struct nvmeibs_client *cl = NR2C(nrch);
		u64 req_tag = be64_to_cpu(req_hdr_tag);
		u16 index = nordda_tag_decode_index(req_tag);
		if (index < cl->nrch_ioreq_num) {
			unsigned long flags;
			spin_lock_irqsave(&nrch->spinlock, flags);
			if (unlikely(!test_and_clear_bit(index, nrch->underway_cmds_bmp))) {
				_NE(error_s_nordda_cmd_ended_not_underway,
					"nrch @NRCH_NAME (@NRCH), idx @INDEX, not underway",
					nrch->name, nrch, (int)index);
				BUG_ON(1);
			}
			spin_unlock_irqrestore(&nrch->spinlock, flags);
		} else {
			_NE(error_cmd_ended_catch_underway_inv_index,
			    "NRCH @NRCH - Invalid request index @IDX_LLONG (max supported index is @NRCH_IOREQ_NUM)",
			    nrch, index, cl->nrch_ioreq_num);
			BUG(); /* There is also a BUG_ON in new_cmd_underway so we should never get here */
		}
	} while(0);
#else
	(void)req_hdr_tag;
#endif
	if (!atomic_dec_return(&nrch->underway_cmds) && nrch->underway_cmds_comp)
		complete(nrch->underway_cmds_comp);
	NFOUT;
}

static void wait_for_all_underway(struct nvmeibs_nr_channel *nrch)
{
	DECLARE_COMPLETION_ONSTACK(underway_cmds_comp);
	NFIN;

	nrch->underway_cmds_comp = &underway_cmds_comp;
	if (atomic_dec_return(&nrch->underway_cmds)) {
		wait_for_completion(nrch->underway_cmds_comp);
	}
	nrch->underway_cmds_comp = NULL;
	BUG_ON(atomic_read(&nrch->underway_cmds));
#ifdef NVMEIBS_NR_CATCH_CMD_ALREADY_UNDERWAY
	BUG_ON(!bitmap_empty(nrch->underway_cmds_bmp, NVMEIB_MAX_NORDDA_IO_REQ));
#endif
	__NFOUT;
}

static int prepare_rdma_write(struct nvmeibs_nr_cmd *cmd)
{
	struct volume_client_io_req_base *io_req = &cmd->cmd_req->io_req.base;
	struct nvmeib_indirect_buf *indirect_hdr;
	struct nvmeib_direct_buf *buf;
	int i, io_len;

	NFIN;

	/* the whole command read collapsed into a single entry */
	if (likely(io_req->buf_dref.contents_magic == NVMEIB_IO_REQ_DIRECT_BUF_MAGIC)) {
		buf = vex_dref_memory(&io_req->buf_dref);
		buf->va = be64_to_cpu(buf->va);
		io_len = buf->len = be32_to_cpu(buf->len);
		buf->key = be32_to_cpu(buf->key);
	}
	else {
		BUG_ON(io_req->buf_dref.contents_magic != NVMEIB_IO_REQ_INDIRECT_BUF_MAGIC);
		indirect_hdr = vex_dref_memory(&io_req->buf_dref);
		indirect_hdr->table_count = be16_to_cpu(indirect_hdr->table_count);
		buf = indirect_hdr->desc_list;
		/* convert each element of the S/G list */
		for (i = 0; i < indirect_hdr->table_count; ++i, ++buf) {
			buf->va = be64_to_cpu(buf->va);
			buf->len = be32_to_cpu(buf->len);
			buf->key = be32_to_cpu(buf->key);
		}
		io_len = indirect_hdr->len = be32_to_cpu(indirect_hdr->len);
	}
	NFOUT;
	return io_len;
}

static struct nvmeib_direct_buf * get_bufs_nbufs(
	struct nvmeibs_nr_cmd *cmd, int *nbufs, int *len)
{
	struct volume_client_io_req_base *io_req = &cmd->cmd_req->io_req.base;
	struct nvmeib_indirect_buf *indirect_hdr;
	struct nvmeib_direct_buf *ret;

	NFIN;
	if (likely(io_req->buf_dref.contents_magic == NVMEIB_IO_REQ_DIRECT_BUF_MAGIC)) {
		ret = vex_dref_memory(&io_req->buf_dref);
		*nbufs = 1;
		*len = ret->len;
		_ND(trace_nordda_get_bufs_nbufs, "len=@LEN", *len);
	} else {
		BUG_ON(io_req->buf_dref.contents_magic != NVMEIB_IO_REQ_INDIRECT_BUF_MAGIC);
		indirect_hdr = vex_dref_memory(&io_req->buf_dref);
		ret = indirect_hdr->desc_list;
		*nbufs = indirect_hdr->table_count;
		*len = indirect_hdr->len;
		_ND(trace_1_nordda_get_bufs_nbufs, "len=@LEN", *len);
	}
	NFOUT;
	return ret;
}

static int read_map_sg_to_ib_sge(struct nvmeib_iu *iu,
	struct nvmeib_direct_buf *bufs, int nbufs, u64 read_from_addr, u32 lkey,
	int n_md_descs, int md_entry_size, bool reset)
{
	int i;
	struct nvmeib_rdma_iu *riu;
	struct nvmeib_direct_buf *pbuf;
	u64 laddr = read_from_addr;
	int rv = 0;

	NFIN;
	if (reset)
		iu->n_rdma_iu = 0;
	if (iu->n_rdma_iu + nbufs + n_md_descs > iu->max_rdma_iu) {
		_NE(error_nordda_read_map_sg_to_ib_sge, "Read OP SG is too big needs @N_RDMA_IU (md @N_MD_DESCS), max=@MAX",
			iu->n_rdma_iu, n_md_descs, iu->max_rdma_iu);
		iu->n_rdma_iu = 0;
		rv = -ENOMEM;
		goto out;
	}

	pbuf = bufs;
	riu = iu->rius + iu->n_rdma_iu;
	for (i = 0; i < nbufs; ++i, ++pbuf, ++riu) {
		riu->raddr = pbuf->va;
		riu->rkey = pbuf->key;
		riu->lkey = pbuf->okey;
		riu->sge_cnt = 1;
		riu->sge[0].addr = laddr;
		riu->sge[0].length = pbuf->len;
		riu->sge[0].lkey = lkey;
		riu->len = pbuf->len;
		_ND(trace_nordda_read_map_sg_to_ib_sge, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
		_ND(trace_1_nordda_read_map_sg_to_ib_sge, "@IDX: addr=@ADDR, len=@LEN, key=@KEY_INT", i,
			riu->sge[0].addr, riu->sge[0].length, riu->sge[0].lkey);
		laddr += riu->sge[0].length + md_entry_size;
	}

	iu->n_rdma_iu += nbufs;

out:
	NFOUT;
	return rv;
}

static int map_inline_md(struct nvmeib_md_desc *md_desc,
	struct nvmeibs_nr_cmd *cmd, int nbufs, int sw_sector_len,
	int n_md_descs, int md_entry_size)
{
	struct nvmeib_iu *iu = cmd->io_iu;
	struct nvmeib_rdma_iu *riu = iu->rius + nbufs;
	u64 laddr = cmd->bb_map->ioaddr;
	u32 lkey = cmd->bb_map->lkey;
	u64 raddr = be64_to_cpu(md_desc->raddr);
	u32 rkey = be32_to_cpu(md_desc->rkey);
	int i;

	NFIN;
	for (i = 0; i < n_md_descs; ++i, ++riu) {
		laddr += sw_sector_len;
		riu->raddr = raddr;
		riu->rkey = rkey;
		riu->sge_cnt = 1;
		riu->sge[0].addr = laddr;
		riu->sge[0].length = md_entry_size;
		riu->sge[0].lkey = lkey;
		riu->len = md_entry_size;
		_ND(trace_nordda_map_inline_md, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
		_ND(trace_1_nordda_map_inline_md, "@IDX: addr=@ADDR, len=@LEN, key=@KEY_INT", i,
			riu->sge[0].addr, riu->sge[0].length, riu->sge[0].lkey);
		laddr += md_entry_size;
		raddr += md_entry_size;
	}
	NFOUT;
	return 0;
}

static int map_sep_md(struct nvmeib_md_desc *md_desc,
	struct nvmeibs_nr_cmd *cmd, int nbufs, int md_size)
{
	struct nvmeib_iu *iu = cmd->io_iu;
	struct nvmeib_rdma_iu *riu = iu->rius + nbufs;
	u64 laddr = cmd->md.phys;
	u32 lkey = cmd->md.lkey;
	u64 raddr = be64_to_cpu(md_desc->raddr);
	u32 rkey = be32_to_cpu(md_desc->rkey);

	NFIN;
	riu->raddr = raddr;
	riu->rkey = rkey;
	riu->sge_cnt = 1;
	riu->sge[0].addr = laddr;
	riu->sge[0].length = md_size;
	riu->sge[0].lkey = lkey;
	riu->len = md_size;
	_ND(trace_nordda_map_sep_md, "raddr=@RADDR, rkey=@RKEY", riu->raddr, riu->rkey);
	_ND(trace_1_nordda_map_sep_md, "addr=@ADDR, len=@LEN, key=@KEY_INT",
		riu->sge[0].addr, riu->sge[0].length, riu->sge[0].lkey);
	NFOUT;
	return 0;
}

static void poll_send_cq_intr(struct nvmeibs_nr_channel *nrch);
static int send_io_cmd_rdma(struct nvmeibs_nr_cmd *cmd, int n_md_descs)
{
	struct nvmeib_send_wr *wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct nvmeib_rdma_iu *riu;
	int n_wrs;
	int i, rv = 0;
	bool retried;
	size_t len_rdma = 0;
	int send_flags = 0;

	NFIN;

	n_wrs = cmd->io_iu->n_rdma_iu + n_md_descs;

	if (!n_wrs) {
		_NW(warn_s_nordda_send_io_cmd_rdma_no_wrs,
			"n_wrs=0 - nrch @NRCH_NAME (@NRCH): cmd: @NR_CMD", cmd->nrch->name, cmd->nrch, cmd);
		goto out;
	}

	/* max_send_wrs includes the rdma-send wr of the io-rsp and n_wrs is only
	   rdma-write wrs, but as we post send here we check against entire send-q/cq size */
	if (unlikely(n_wrs > cmd->nrch->max_send_wrs)) {
		_NW(warn_1_nordda_send_io_cmd_rdma, "nrch @NRCH_NAME: n_wrs=@N_WRS (@N_RDMA_IU + @N_MD_DESCS) but max=@MAX ",
		   cmd->nrch->name, n_wrs, cmd->io_iu->n_rdma_iu, n_md_descs,
		   cmd->nrch->max_send_wrs);
		/* if we'll send this many wrs next we may never succeed as send-q and send-cq is too small */
		WARN_ON_ONCE(1);
		rv = -1;
		goto out;
	}

	if (unlikely(n_wrs > cmd->io_iu->max_rdma_iu)) {
		_NE(error_1_nordda_send_io_cmd_rdma,
			"nrch @NRCH_NAME: No use in dynamically allocating @INT WRs "
			"as it exceeds max-rdma-ius=@UINT",
			cmd->nrch->name, n_wrs, cmd->io_iu->max_rdma_iu);
		rv = -1;
		goto out;
	}

	if (unlikely(n_wrs > ARRAY_SIZE(cmd->wr))) {
		if (!(wr = kzalloc(sizeof(*wr) * n_wrs, GFP_ATOMIC))) {
			_NE(error_nordda_send_io_cmd_rdma, "Failed to allocate WQE for send");
			rv = -1;
			goto out;
		}
	}
	else {
		wr = cmd->wr;
		memset(wr, 0, sizeof(*wr) * n_wrs);
	}

#if ENABLE_SIW
	if (P2NV(cmd->nrch->net->params.port)->dev_type == DT_siw)
		send_flags |= SIW_IB_SEND_MORE_WQES | SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	/* Add WRs of data read from disk */
	i = 0;
	riu = cmd->io_iu->rius;
	BUG_ON(n_wrs > NVMEIB_MAX_RDMA_UI);
	for (i = 0; i < n_wrs - 1; ++i, ++riu) {
		nvmeib_send_wr_common(wr[i]).opcode = IB_WR_RDMA_WRITE;
		nvmeib_send_wr_common(wr[i]).wr_id =
				nvmeib_encode_wr_id(NVMEIB_RDMA_MID_0 + i, cmd->io_iu->index);
		nvmeib_send_wr_rdma(wr[i]).remote_addr = riu->raddr;
		nvmeib_send_wr_rdma(wr[i]).rkey = riu->rkey;
		nvmeib_send_wr_common(wr[i]).num_sge = riu->sge_cnt;
		nvmeib_send_wr_common(wr[i]).sg_list = riu->sge;
		nvmeib_send_wr_common(wr[i]).send_flags = send_flags;
		nvmeib_send_wr_set_next(wr[i], &wr[i + 1]);
		len_rdma += riu->len;
	}
	nvmeib_send_wr_common(wr[i]).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr[i]).wr_id =
		nvmeib_encode_wr_id(NVMEIB_RDMA_LAST, cmd->io_iu->index);
	nvmeib_send_wr_rdma(wr[i]).remote_addr = riu->raddr;
	nvmeib_send_wr_rdma(wr[i]).rkey = riu->rkey;
	nvmeib_send_wr_common(wr[i]).num_sge = riu->sge_cnt;
	nvmeib_send_wr_common(wr[i]).sg_list = riu->sge;
	nvmeib_send_wr_common(wr[i]).send_flags = send_flags;
	len_rdma += riu->len;

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibs_nr_skip_rdma_write_back)) {
		rv = 0;
	}
	else
#endif
	{
	retried = false;
retry:
	if (!retried && (rv = nvmeibs_ib_post_send(
		cmd->nrch->net, nvmeib_send_wr_to_ib_ptr(*wr), &bad_wr)) < 0) {
		_NT(trace_nordda_send_io_cmd_rdma, "Fail to send io command on admin channel, n_wrs=@N_WRS", n_wrs);
		if (rv == -ENOMEM) {
			retried = true;
			poll_send_cq_intr(cmd->nrch);
			goto retry;
		}
	}
	if (rv) {
		nvmeibs_net_release(cmd->nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_SND_IO_CMD_FAILED);
	}
	}
	if (unlikely(wr != cmd->wr))
		kfree(wr);

out:
	if (!rv)
		rv = len_rdma;
	NFOUT;
	return rv;
}

static struct nvmeib_iu *get_recv_iu(struct nvmeibs_nr_channel *nrch, u32 index)
{
	struct nvmeib_iu *iu = NULL;
	if (N2SI(nrch->net)) {
		iu = nvmeib_srq_rtrv_recv(N2SI(nrch->net), index, nrch->net);
		BUG_ON(iu->priv != N2SI(nrch->net));
	}
	else if (nrch->recv_q)
		iu = nvmeib_rq_rtrv_recv(nrch->recv_q, index);
	return iu;
}

static int post_recv_iu(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *iu)
{
	int rv;

	if (!iu) {
		rv = 0;
		goto out;
	}

	nvmeibs_nr_lat_meas_on_post_recv(iu);

	if (N2SI(nrch->net)) {
		rv = nvmeib_srq_post_recv(N2SI(nrch->net), iu);
	} else if (nrch->recv_q)
		rv = nvmeibs_net_post_recvq(nrch->net, nrch->recv_q, iu);
	else {
		_NE(error_nordda_post_recv_iu, "No SRQ or RQ available");
		rv = -EINVAL;
	}

out:
	return rv;
}


static int io_req_jmd_piggyback(struct nvmeibs_nr_channel *nrch, struct nvmeibs_client *cl,
	const struct volume_client_io_req_base *io_req,
	union jblock_md *jmdc_ent, int sw_sector_len)
{
	u16 idx;
	int rv = -1;
	void *p;
	NFIN;

	if (unlikely(!jmdc_ent)) {
		_NE(error_nordda_io_req_jmd_piggyback, "Journal MD piggyback NULL");
		goto out;
	}
	if (unlikely(io_req->ind_op != NVMEIB_IND_OP_IO_WRITE)) {
		_NT(trace_nordda_io_req_jmd_piggyback, "Journal MD piggyback err, invalid ind-op=@IND_OP",
		   io_req->ind_op);
		goto out;
	}
	if (unlikely(!cl->di->mtdt_extd && io_req->md_desc.size == 0)) {
		_NT(trace_1_nordda_io_req_jmd_piggyback, "Journal MD piggyback err, no md-buffer");
		goto out;
	} else if (cl->di->mtdt_extd) {
		_NT(trace_2_nordda_io_req_jmd_piggyback, "TBD: Extract Metadata from IO Buffer!");
//#warning	JMD Piggyback with Inline MD Not Implemented
		rv = 0;
		goto out;
	}
	idx = (u16)(io_req->jmd_pb[0]) - 1;
	if (unlikely(idx > cl->jrnl_rng_n_ent)) {
		_NT(trace_3_nordda_io_req_jmd_piggyback, "Journal MD piggyback err, idx @IDX overflow",
		   idx);
		goto out;
	}
	p = jmdc_ent;
	p += sw_sector_len;
	jmdc_ent = p;
	rv = nvmeibs_serjio_jmdc_entry_set(nrch->jrange_handle, 0, 1, &idx, NULL, jmdc_ent, true);

out:
	NFOUT;
	return rv;
}

static void get_md_sw_params(struct nvmeibs_disk_info *di,
	const struct volume_client_io_req_base *io_req,
	int *sw_sector_shift, int *md_sw_entry_len)
{
	NFIN;
	*sw_sector_shift = be32_to_cpu(io_req->md_desc.sw_sector_shift);
	*md_sw_entry_len = di->metadata << (*sw_sector_shift - di->block_shift);
	NFOUT;
}

static int handle_cb_md_rd_mod_wr(struct nvmeibs_client *cl,
	struct nvmeibs_nr_cmd *cmd, struct nvmeibs_disk_info *di)
{
	struct volume_client_io_req_base *io_req = &cmd->cmd_req->io_req.base;
	int hw_sectors __attribute__((unused));
	int sw_sector_shift;
	int sw_sector_len;
	int io_len;
	int md_sw_entry_len;
	u64 rmw_action;
	int rv = 0;
	int n_md_descs;
	int i;
	void *p;

	NFIN;
	io_len = be64_to_cpu(io_req->data_len);
	hw_sectors = io_len >> di->block_shift;
	rmw_action = be64_to_cpu(io_req->md_desc.rmw_action);
	get_md_sw_params(di, &cmd->cmd_req->io_req.base, &sw_sector_shift, &md_sw_entry_len);
	sw_sector_len = 1 << sw_sector_shift;
	if (di->mtdt_extd) {
		struct nvmeib_buffer bb_buf;
		nvmeib_buffer_init_sgl(&bb_buf, cmd->bb_map->mem_table.sgl,
				       cmd->bb_map->mem_table.nents, io_len, 0);
		nvmeib_buffer_copy_md_ext_to_buffer(&bb_buf, sw_sector_len, md_sw_entry_len, cmd->md.virt, cmd->md.size);
	}
	n_md_descs = io_len >> sw_sector_shift;
	p = cmd->md.virt;
	for (i = 0; i < n_md_descs; ++i) {
		nvmeib_block_dp_ec_dmd_read_mod_wr(p, rmw_action);
		p += md_sw_entry_len;
	}
	cmd->req.metadata = cmd->md.virt;
	NFOUT;
	return rv;
}

static inline int verify_rd_cmd_buf_dref(const struct vex_dref *buf_dref, __be16 version_tag)
{
	int rv;
	if (unlikely(buf_dref->version_tag != version_tag)) {
		_NE(verify_rd_cmd_buf_dref_e1, "Invalid direct buf version tag @UINT", be16_to_cpu(buf_dref->size));
		rv = -EPROTO;
		goto out;
	}
	if (likely(buf_dref->contents_magic == NVMEIB_IO_REQ_DIRECT_BUF_MAGIC)) {
		if (unlikely(buf_dref->size != cpu_to_be32(sizeof(struct nvmeib_direct_buf)))) {
			_NE(verify_rd_cmd_buf_dref_e2, "Invalid direct buf dref size @UINT", be32_to_cpu(buf_dref->size));
			rv = -EPROTO;
			goto out;
		}
	} else {
		if (buf_dref->contents_magic != NVMEIB_IO_REQ_INDIRECT_BUF_MAGIC) {
			_NE(verify_rd_cmd_buf_dref_e3, "Invalid buf dref magic @HEX (@_X) in io_req",
			   (const char *)buf_dref->contents_magic,
			   be64_to_cpu(buf_dref->contents_magic));
			rv = -EPROTO;
			goto out;
		}
	}
	rv = 0;
out:
	return rv;
}

static int handle_cb_read_md(struct nvmeibs_client *cl,
	struct nvmeibs_nr_channel *nrch, struct nvmeibs_nr_cmd *cmd,
	struct nvmeibs_disk_info *di)
{
	struct volume_client_io_req_base *io_req = &cmd->cmd_req->io_req.base;
	struct nvmeib_direct_buf *bufs = NULL;
	int nbufs = 0;
	int n_md_descs = 0;
	int sw_sector_shift = 0;
	int md_entry_size = 0;
	int md_size = 0;
	int sep_md_size = 0;
	int len = 0;
	int rv = 0;

	NFIN;
	cmd->io_iu->n_rdma_iu = 0;
	n_md_descs = 0;
	md_size = be32_to_cpu(io_req->md_desc.size);
	if (io_req->ind_op == NVMEIB_IND_OP_IO_READ) {
		if (verify_rd_cmd_buf_dref(&io_req->buf_dref, cmd->req_version_tag) < 0) {
			rv = -1;
			goto out;
		}
		prepare_rdma_write(cmd);
		bufs = get_bufs_nbufs(cmd, &nbufs, &len);
		if (io_req->md_desc.size) {
			if (di->mtdt_extd) {
				get_md_sw_params(di, &cmd->cmd_req->io_req.base,
					&sw_sector_shift, &md_entry_size);
				n_md_descs = nbufs;
			}
			else {
				md_entry_size = 0;
				n_md_descs = 1;
			}
		}
	}
	else if (io_req->ind_op == NVMEIB_IND_OP_MD_READ) {
		bufs = 0;
		nbufs = 0;
		if (di->mtdt_extd) {
			get_md_sw_params(di, &cmd->cmd_req->io_req.base,
				&sw_sector_shift, &md_entry_size);
			n_md_descs = md_size / md_entry_size;
		}
		else {
			md_entry_size = 0;
			n_md_descs = 1;
		}
	}
	if ((rv = read_map_sg_to_ib_sge(cmd->io_iu, bufs, nbufs,
		cmd->bb_map->ioaddr, cmd->bb_map->lkey, n_md_descs, md_entry_size, true))) {
		_NT(trace_nordda_handle_cb_read_md, "Fail to prepare RDMA data");
		rv = -1;
		goto out;
	}
	if (di->metadata) {
		if (di->mtdt_extd)
			map_inline_md(&io_req->md_desc, cmd, nbufs, 1 << sw_sector_shift,
				n_md_descs, md_entry_size);
		else {
			map_sep_md(&io_req->md_desc, cmd, nbufs, md_size);
			sep_md_size = md_size;
		}
	}
	if ((rv = send_io_cmd_rdma(cmd, n_md_descs)) < 0) {
		_NT(trace_1_nordda_handle_cb_read_md, "Fail to send RDMA data");
		rv = -1;
		goto out;
	}
	if (unlikely(rv != len + sep_md_size)) {
		_NE(bug_s_nordda_handle_cb_read_md_len_mismatch,
			"BUG - Length Mismatch: cmd: @NR_CMD rv: @RV len: @LEN n_md_descs: @N_MD_DESCS md_entry_size: @MD_ENTRY_SIZE\n",
		   cmd, rv, len, n_md_descs, md_entry_size);
		BUG_ON(1);
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

struct io_rsp_ctx {
	struct nvmeibs_nr_cmd *cmd;
	struct volume_client_io_req *io_req;
	enum nvmeib_wr_opcode send_wr_opcode;
	int rsp_opcode;
};

static inline int nrch_send_rsp(struct nvmeibs_client *cl,
	struct nvmeibs_nr_channel *nrch, u8 opcode, u64 tag, u16 version_tag,
	struct nvmeib_iu *send_ioctx, void *p, int len, int wr_opcode, u16 wr_version)
{
	bool retried = false;
	int rv;
	__NFIN;

retry:
	rv = nvmeibs_client_send_rsp(cl, nrch->net, opcode, tag, version_tag,
								 send_ioctx, p, len, wr_opcode, wr_version);
	if (rv) {
		 if (rv == -ENOMEM && !retried) {
			_NT(trace_nordda_nrch_send_rsp, "nrch @NRCH_NAME: scq overflow", nrch->name);
			retried = true;
			poll_send_cq_intr(nrch);
			goto retry;
		 }
		 else {
			 _NT(trace_1_nordda_nrch_send_rsp, "nrch @NRCH_NAME: Fail to send-rsp (rv=@RV)", nrch->name, rv);
			 nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_SND_RSP_FAILED);
		 }
	}

	__NFOUT;
	return rv;
}

/* WAS: io_cmd_cb_base */
VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_rsp_srv_base_encode)
{
	int rv;
	struct io_rsp_ctx *rsp_ctx = arg;
	struct nvmeibs_nr_cmd *cmd = rsp_ctx->cmd;
	struct volume_client_io_req *io_req = rsp_ctx->io_req;
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct nvmeibs_disk_info *di;
	struct volume_server_io_rsp_base *io_rsp = wire_buf;

	struct nvmeibs_disk_lock_mem_info *lmi;
	u64 offset;
	int virt_index;
	int virt_entry;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*io_rsp) > wire_buf_end);

	memset(io_rsp, 0, sizeof(*io_rsp));

	di = cl->di;

	/* we made it - we manage to complete successfully io comamnd on disk -
	   so now in the case of read we need to send back the data
	*/
	if (io_req->base.ind_op == NVMEIB_IND_OP_IO_READ ||
		io_req->base.ind_op == NVMEIB_IND_OP_MD_READ) {
		if ((rv = handle_cb_read_md(cl, nrch, cmd, di))) {
			io_rsp->comp_code = cpu_to_be32(NVMEIBS_IO_RSP_ERR_RDMA);
			_NE(vex_nrch_io_rsp_srv_base_encode_e_cb_read, "Failed handle-cb_read_md");
			goto out;
		}
		rsp_ctx->send_wr_opcode = NVMEIB_REPLY_IO_W_RDMA;
		rsp_ctx->rsp_opcode = NVMEIBS_RSP_IO_OPCODE_OK;
	} else {
		rsp_ctx->send_wr_opcode = NVMEIB_REPLY_IO;
		rsp_ctx->rsp_opcode = NVMEIBS_RSP_IO_OPCODE_OK;
	}

	/* sent the piggyback read lock/ write db if needed */
	if (io_req->base.lmi) {
		if (!(lmi = nvmeibs_verify_lmi(cl->di->priv, io_req->base.lmi))) {
			_NE(vex_nrch_io_rsp_srv_base_encode_e1, "Invalid lmi val @_X for piggy-back lock", io_req->base.lmi);
			io_rsp->comp_code = cpu_to_be32(NVMEIBS_IO_RSP_ERR_PB_LOCK);
			goto out;
		}
		offset = be64_to_cpu(io_req->base.offset);
		virt_index = offset >> PAGE_SHIFT;
		virt_entry = offset - (virt_index << PAGE_SHIFT);
		if (io_req->base.ind_pg_op == NVMEIB_IND_PG_READ_LOCK) {
			io_rsp->piggyback_read =
				*(u64 *)(page_address(lmi->pages[virt_index]) + virt_entry);
		} else if (io_req->base.ind_pg_op == NVMEIB_IND_PG_WRITE_BLKSET_INFO) {
			/* write to blkset info (upper 32-bits of lock) */
			union nvmeib_lock_blkset_entry *lock_ptr = (page_address(lmi->pages[virt_index]) + virt_entry);
			lock_ptr->blkset_info.all = be32_to_cpu(io_req->base.blkset_info);
			io_rsp->piggyback_read = lock_ptr->all;
		} else {
			_NE(error_nordda_io_cmd_cb_base, "Invalid piggy-back operation @IND_PG_OP",
				io_req->base.ind_pg_op);
		}
	}

	/* update io-ka from piggy-back */
	*nrch->rionic->keep_alive.ka_ctr = be64_to_cpu(io_req->base.io_ka_val);
	/* JH IOMMU: Changed to DMA_FROM_DEVICE. dir must be same as map/unmap */
	ib_dma_sync_single_for_device(
		P2IB(nrch->net->params.port), nrch->rionic->keep_alive.ka_ctr_phys_addr,
			sizeof(*nrch->rionic->keep_alive.ka_ctr), DMA_FROM_DEVICE);
	io_rsp->comp_code = 0;

out:
	return sizeof(*io_rsp);
}

static inline void cmd_stats_md_on_cmd_process(struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_nr_cmd_stats_md *stats_md_head = &CMD_STATS_MD_HEAD(cmd);

	__NFIN;
	/* Clear the slot we are about to start using */
	memset(stats_md_head, 0, sizeof(*stats_md_head));
	if (!cmd->gen_cmd) {
		switch(cmd->ind_op) {
		case NVMEIB_IND_OP_IO_READ:
			stats_md_head->stat_verb = cmd->req.stats.is_recovery? 
				IO_STAT_VERB_RECOV_READ : IO_STAT_VERB_READ;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = cmd->req.data_len;
			break;
		case NVMEIB_IND_OP_IO_WRITE:
			stats_md_head->stat_verb = cmd->req.stats.is_recovery? 
				IO_STAT_VERB_RECOV_WRITE : IO_STAT_VERB_WRITE;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = cmd->req.data_len;
			break;
		case NVMEIB_IND_OP_IO_DSM:
			stats_md_head->stat_verb = IO_STAT_VERB_DISCARD;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = cmd->req.data_len;
			break;
		case NVMEIB_IND_OP_MD_READ:
			stats_md_head->stat_verb = cmd->req.stats.is_recovery? 
				IO_STAT_VERB_RECOV_READ : IO_STAT_VERB_READ;
			stats_md_head->stat_verb = cmd->req.stats.is_recovery? 
				IO_STAT_VERB_RECOV_READ : IO_STAT_VERB_READ;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = cmd->req.mtdt_size;
			break;
		case NVMEIB_IND_OP_MD_RD_MOD_WR:
			stats_md_head->stat_verb = cmd->req.stats.is_recovery? 
				IO_STAT_VERB_RECOV_WRITE : IO_STAT_VERB_WRITE;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = cmd->req.mtdt_size;
			break;
		case NVMEIB_IND_OP_IO_WRITE_UNCOR:
			/* Nothing is transferred for this, so don't count it */
			stats_md_head->stat_verb = N_IO_STAT_VERBS;
			CMD_STATS_MD_HEAD(cmd).io_xfer_size = 0;
			break;
		default:
			BUG();
			break;
		}
	} else {
		stats_md_head->stat_verb = IO_STAT_VERB_GEN_TX;
	}

	stats_md_head->version = cmd->version;
	stats_md_head->ch_version = cmd->ch_version;
	__NFOUT;
}

static inline void cmd_stats_md_on_send_rsp(struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_nr_cmd_stats_md *stats_md_head = &CMD_STATS_MD_HEAD(cmd);

	__NFIN;
	stats_md_head->send_rsp_time = nvmeib_public_ktime_get();
	
	/* Set to valid and increment the producer */
	stats_md_head->valid = true;
	cmd->stats_md_prod++;
	__NFOUT;
}

static void io_cmd_cb(void *arg, int status, u32 result)
{
	struct nvmeibs_nr_cmd *cmd = arg;
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct volume_client_io_req *io_req = &cmd->cmd_req->io_req;

	/* TBD: Allocate based on vex_ext  */
	struct volume_server_io_rsp_base io_rsp;
	void *payload = &io_rsp;
	const void *payload_end = &io_rsp + 1;
	int rv = 0;
	struct io_rsp_ctx rsp_ctx = {
		.rsp_opcode = NVMEIBS_RSP_IO_OPCODE_ERR,
		.send_wr_opcode = NVMEIB_REPLY_IO /* we use cmd->io_iu anyhow */ };

#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
	u64 n_cb, n_submit = atomic64_read(&cmd->n_submit);
	struct nvmeib_stack_trace st = {
		.max_entries = ARRAY_SIZE(cmd->cb_ents),
		.entries = cmd->cb_ents,
		.skip = 0,
	};
#endif

	__NFIN;

	/* [NVMESH-2935]: Moved to nvmeibs_nvme.c using trampoline remote_iops_stats_cb
	 * nvmeibs_disk_record_stats(cl->di, &cmd->req, status);
	 */
	nvmeibs_nr_lat_meas_record_io_cmd_cb(nrch, cmd);

	io_rsp.piggyback_read = NVMEIBS_NORDDA_PIGGYBACK_READ_POISON;

#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
	if (unlikely(n_cb = atomic64_inc_return(&cmd->n_cb)) > n_submit) {
		_NE(error_nordda_io_cmd_cb_double_cb,
			"DOUBLE CALLBACK - disk: @DISK_ID_STR (@DISK) nrch: @NRCH_NAME (@NRCH)"
			" cmd: @CMD_PTR. Prev Callstack @FN <- @FN <- @FN <- @FN <- @FN",
			cl->di->disk_id, cl->di, nrch->name, nrch,
			cmd, (void *)cmd->cb_ents[0], (void *)cmd->cb_ents[1],
			(void *)cmd->cb_ents[2], (void *)cmd->cb_ents[3], (void *)cmd->cb_ents[4]);
		BUG_ON(1);
	}
	nvmeib_public_save_stack_trace(&st);
#endif

	/* TBD: Remove with dynamic payload size */
	BUG_ON(cl->vex_io_rsp_ops->vex_ext != vex_base);
	_ND(trace_nordda_io_cmd_cb, "--- Start handling NVMEIBC_IO_CMD callback, nrch @NRCH_NAME (@CH_PTR),"
			" req_index @REQ_INDEX, req_version @VERSION", nrch->name, nrch, cmd - nrch->io_cmds, cmd->version);
	if (!nvmeibs_net_inc(nrch->net)) {
		goto dismiss;
	}

	//cmd->recv_ioctx = NULL; - moved to right before sending rsp
	if (status) {
		_NT(trace_1_nordda_io_cmd_cb, "IO command op=@IND_OP on disk @DISK_NAME nrch @NRCH_NAME failed result = @RESULT_INT status = @STATUS",
			io_req->base.ind_op, io_req->base.disk_name, nrch->name, result, status);
		rv = -1;
		/* TBD: Unify with call to vex_encode */
		io_rsp.comp_code = cpu_to_be32(status);
		goto send_rsp;
	}

	if (unlikely((io_req->base.ind_op == NVMEIB_IND_OP_MD_RD_MOD_WR) &&
				 cmd->req.nvme_op == nvme_cmd_read)) {
		if (!(rv = handle_cb_md_rd_mod_wr(cl, cmd, cl->di))) {
			cmd->req.nvme_op = nvme_cmd_write;
			nvmeibs_nr_lat_meas_record_io_submit(nrch, cmd);
			if ((rv = submit_local_cmd(cl->di, &cmd->req)) < 0) {
					_ND(io_cmd_cb_d1, "Fail to submit cmd to disk (rv = @INT)", rv);
					NRCH_PCPU_STAT_INC(nrch, n_iops_err);
					goto trim_md_txid_err;
			}
			nvmeibs_net_dec(nrch->net);
			goto out;
		}
		else {
trim_md_txid_err:
			/* TBD: Unify with call to vex_encode */
			io_rsp.comp_code = cpu_to_be32(NVMEIBS_IO_RSP_ERR_TXID_TRIM);
			goto send_rsp;
		}
	}

	rsp_ctx.cmd = cmd;
	rsp_ctx.io_req = io_req;
	rv = CALL_VEX_OP(encode,
				vex_nrch_io_rsp_srv_ops, BASE_ONLY,
					base, vex_nrch_io_rsp_srv_base_encode,
						cl->vex_io_rsp_ops, payload, payload_end, &rsp_ctx);
	BUG_ON(rv != sizeof(io_rsp));

send_rsp:
	cmd_ended(nrch, cmd->req_hdr_tag);
	if (!cl->dismissed) {
		atomic_inc(&cmd->n_send_rsp_io);
		_ND(trace_3_nordda_io_cmd_cb, "--- Sending response to NVMEIBC_IO_CMD, nrch @NRCH_NAME (@CH_PTR)"
					" req_index @REQ_INDEX, req_version @VERSION, req_tag @TAG, iu_index @INDEX",
					nrch->name, nrch, cmd - nrch->io_cmds, cmd->version, cmd->req_hdr_tag, cmd->io_iu->index);
		BUG_ON(cmd - cmd->nrch->io_cmds != nordda_tag_decode_index(be64_to_cpu(cmd->req_hdr_tag)));
		BUG_ON(nordda_tag_decode_version_decode_verify_reserved(cmd->version, be64_to_cpu(cmd->req_hdr_tag)));
		nvmeibs_nr_lat_meas_record_send_rsp(nrch, cmd);
		cmd_stats_md_on_send_rsp(cmd);
		nrch_send_rsp_and_post_recv(nrch,
			rsp_ctx.rsp_opcode, //was: rv >= 0 ? rsp_ctx.rsp_opcode : NVMEIBS_RSP_IO_OPCODE_ERR,
			cmd->req_hdr_tag, cmd->req_version_tag, cmd->io_iu, payload,
			payload_end - payload, rsp_ctx.send_wr_opcode, cmd->version, cmd->recv_ioctx);
	} else {
		post_recv_iu(nrch, cmd->recv_ioctx);
	}
	nvmeibs_net_dec(nrch->net);
	goto out;

	_ND(trace_2_nordda_io_cmd_cb, "--- Finish handling NVMEIBC_IO_CMD callback, nrch @NRCH_NAME (@CH_PTR)"
			" req_index @REQ_INDEX, req_version @VERSION", nrch->name, nrch, cmd - nrch->io_cmds, cmd->version);

dismiss:
	cmd_ended(nrch, cmd->req_hdr_tag);
	post_recv_iu(nrch, cmd->recv_ioctx);
out:
	cmd->recv_ioctx = NULL;
	__NFOUT;
}

static void io_cmd_cb_work(struct workqe_struct *work)
{
	struct nvmeibs_nr_cmd *cmd = container_of(work, struct nvmeibs_nr_cmd, work);
	io_cmd_cb(cmd, 0, 0);
}

static int ind_2_nvme(
    enum nvmeibc_indirect_op ind_op)
{
    int op;
    NFIN;

    switch (ind_op) {
    case NVMEIB_IND_OP_IO_READ:
        op = nvme_cmd_read;
        break;
    case NVMEIB_IND_OP_IO_WRITE:
        op = nvme_cmd_write;
        break;
    case NVMEIB_IND_OP_IO_DSM:
        op = nvme_cmd_dsm;
        break;
	case NVMEIB_IND_OP_IO_WRITE_UNCOR:
		op = nvme_cmd_write_uncor;
		break;
    case NVMEIB_IND_OP_MD_READ:
	case NVMEIB_IND_OP_MD_RD_MOD_WR:
		op = nvme_cmd_read;
        break;
    default:
        _NE(error_nordda_ind_2_nvme, "Unknown indirect io-op @IND_OP", ind_op);
        op = -1;
        break;
    }

    NFOUT;
    return op;
}

struct vex_srv_lock_dec_ctx {
	struct nvmeibs_client *cl;
	struct nvmeib_gen_cmd_lock_param *lock_param;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_srv_lock_req_base_decode)
{
	const struct volume_client_lock_req_base *lock_req = wire_buf;
	struct vex_srv_lock_dec_ctx *ctx = arg;
	struct nvmeibs_client *cl = ctx->cl;
	struct nvmeibs_disk_private_data *disk_pd = cl->di->priv;
	struct nvmeib_gen_cmd_lock_param *lock_param = ctx->lock_param;
	struct nvmeibs_disk_lock_mem_info *lmi;
	ssize_t rv;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*lock_req) > wire_buf_end);
	lock_param->op = lock_req->op;

	if (link_ext == vex_base) {
		/* For extension >= vex_ext1, we use the seg_id */

		/* Verify LMI before using the pointer directly */
		if (!(lmi = nvmeibs_verify_lmi(disk_pd, lock_req->lmi))) {
			_NE(vex_nrch_io_srv_lock_req_base_decode_e1, "Invalid lmi val @_X", lock_req->lmi);
			rv = -ENOENT;
			goto out;
		}
		lock_param->seg_id = lmi->seg_id;
	}
	lock_param->offset = be64_to_cpu(lock_req->offset);
	lock_param->atomic.compare_add = be64_to_cpu(lock_req->atomic.compare_add);
	lock_param->atomic.compare_add_mask = be64_to_cpu(lock_req->atomic.compare_add_mask);
	lock_param->atomic.swap = be64_to_cpu(lock_req->atomic.swap);
	lock_param->atomic.swap_mask = be64_to_cpu(lock_req->atomic.swap_mask);

	rv = sizeof(*lock_req);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_srv_lock_req_ext1_decode)
{
	const struct volume_client_lock_req_ext1 *lock_req = wire_buf;
	struct vex_srv_lock_dec_ctx *ctx = arg;
	struct nvmeib_gen_cmd_lock_param *lock_param = ctx->lock_param;
	ssize_t rv;

	if (!wire_buf) {
		if (lock_param->op == NVMEIB_LOCK_RDMA_READ ||
			lock_param->op == NVMEIB_LOCK_RDMA_WRITE) {
			_NE(vex_nrch_io_srv_lock_req_ext1_decode_e1, "Invalid lock op @INT for base-only request", lock_param->op);
			rv = -EINVAL;
			goto out;
		}
		rv = 0;
		goto out;
	}
	lock_param->seg_id = be64_to_cpu(lock_req->seg_id);
	lock_param->rdma.table_type = lock_req->rdma.table_type;
	lock_param->rdma.len = be32_to_cpu(lock_req->rdma.len);
	lock_param->rdma.data = (void *)lock_req->rdma.data;

	rv = sizeof(*lock_req);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_srv_lock_rsp_base_encode)
{
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = arg;
	struct volume_server_lock_rsp_base *wire_lock_rsp = wire_buf;

	BUG_ON(!wire_lock_rsp);
	BUG_ON(wire_buf + sizeof(*wire_lock_rsp) > wire_buf_end);

	wire_lock_rsp->cmp_swap_val = cpu_to_be64(lock_rsp->cmp_swap_val);
	wire_lock_rsp->comp_code = cpu_to_be32(lock_rsp->comp_code);

	return sizeof(*wire_lock_rsp);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_io_srv_lock_rsp_ext1_encode)
{
	struct nvmeib_gen_cmd_lock_rsp *lock_rsp = arg;
	struct volume_server_lock_rsp_ext1 *wire_lock_rsp = wire_buf;

	BUG_ON(!wire_lock_rsp);
	BUG_ON(wire_buf + sizeof(*wire_lock_rsp) > wire_buf_end);

	/* Fill with POISON value first */
	memset(wire_lock_rsp->read_data, 0xcc, sizeof(lock_rsp->read_data));
	BUG_ON(lock_rsp->read_len > sizeof(wire_lock_rsp->read_data));
	memcpy(wire_lock_rsp->read_data, lock_rsp->read_data, lock_rsp->read_len);
	wire_lock_rsp->read_len = cpu_to_be32(lock_rsp->read_len);

	return sizeof(*wire_lock_rsp);
}

static inline struct nvmeib_iu * nordda_send_ioctx_get_or_disconnect(struct nvmeibs_net *net) {
	struct nvmeib_iu *ret;

	if (!(ret = nvmeibs_net_get_ioctx(net))) {
		_NE(nordda_send_ioctx_get_or_disconnect_no_rsrcs, "@NET has no ioctx resource available - release net", net);
		nvmeibs_net_release(net, NVMEIBS_LOGOUT_REASON_NR_CH_NO_SEND_IOCTX_RSRC);
	}
	return ret;
}

static void execute_lock(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	const void *req_buf_end = recv_ioctx->buf + recv_ioctx->size;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct nvmeibs_disk_private_data *disk_pd = cl->di->priv;
	struct nvmeib_gen_cmd_lock_param lock_param = {};
	struct nvmeib_gen_cmd_lock_rsp lock_rsp = {};
	struct vex_srv_lock_dec_ctx dec_ctx = {
		.lock_param = &lock_param,
		.cl = NR2C(nrch),
	};
	struct volume_server_lock_rsp wire_lock_rsp;
	void *payload = &wire_lock_rsp;
	int payload_len = sizeof(wire_lock_rsp);
	int rv;
	u64 req_tag = be64_to_cpu(req->hdr.tag);
	u16 cmd_idx = nordda_tag_decode_index(req_tag);
	struct nvmeibs_nr_cmd *nr_cmd = &nrch->io_cmds[cmd_idx];
	struct nvmeib_iu *send_ioctx;

	__NFIN;

	if (!(send_ioctx = nordda_send_ioctx_get_or_disconnect(nrch->net))) {
		cmd_ended(nrch, req->hdr.tag);
		goto out;
	}

	if (cmd_idx >= cl->nrch_ioreq_num) {
		_NE(error_nordda_execute_lock, "Invalid request index @REQ_INDEX (max supported index is @NRCH_IOREQ_NUM)",
			(int)cmd_idx, cl->nrch_ioreq_num);
		rv = NVMEIBS_IO_RSP_ERR_INV_TAG;
		goto send_rsp;
	}
	if (be16_to_cpu(req->version_tag) != nrch->io_srv_lock_req_ops->vex_ext) {
		_NT(execute_lock_r1, "Invalid version tag @INT for NVMEIBC_IO_CMD (NVMEIBC_CR_LOCK)", be16_to_cpu(req->version_tag));
		rv = NVMEIBS_IO_RSP_ERR_INV_VER_TAG;
		goto send_rsp;
	}
	nr_cmd->gen_cmd = true;
	nr_cmd->gen_op = NVMEIB_GEN_OP_LOCK;
	nr_cmd->version = nordda_tag_decode_version(req_tag);
	nr_cmd->ch_version = nordda_tag_decode_ch_version(req_tag);
	rv = CALL_VEX_OP(decode,
			vex_nrch_io_srv_lock_req_srv_ops, ONE_EXT,
				base, vex_nrch_io_srv_lock_req_base_decode,
				ext1, vex_nrch_io_srv_lock_req_ext1_decode,
					nrch->io_srv_lock_req_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req),
					req_buf_end, &dec_ctx);
	if (rv < 0) {
		lock_rsp.comp_code = rv;
		goto encode_rsp;
	}

	rv = nvmeibs_execute_lock_gen_cmd(&lock_param, disk_pd, &lock_rsp);
	lock_rsp.comp_code = rv;

encode_rsp:
	rv = CALL_VEX_OP(encode,
				vex_nrch_io_srv_lock_rsp_srv_ops, ONE_EXT,
					base, vex_nrch_io_srv_lock_rsp_base_encode,
					ext1, vex_nrch_io_srv_lock_rsp_ext1_encode,
						nrch->io_srv_lock_rsp_ops, payload, payload + payload_len, &lock_rsp);
	if (rv < 0)
		goto send_rsp;
	payload_len = rv;
	rv = 0;

send_rsp:
	cmd_ended(nrch, req->hdr.tag);
	nrch_send_rsp(cl, nrch,
			rv >= 0 ? NVMEIBS_RSP_IO_OPCODE_OK : NVMEIBS_RSP_IO_OPCODE_ERR,
			req->hdr.tag, req->version_tag, send_ioctx, payload,
			payload_len, NVMEIB_REPLY_NO_IO, nr_cmd->version);

out:
	post_recv_iu(nrch, recv_ioctx);
	__NFOUT;
}

struct io_cmd_ctx {
	struct nvmeibs_nr_channel *nrch;
	struct nvmeib_iu *recv_ioctx;
	struct nvmeibs_nr_cmd *cmd;
	struct nvmeibs_disk_info *di;
	u64 idx;
};

extern bool nvmeibs_disk_collect_stats;

/* WAS: io_cmd_process_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_req_srv_base_decode)
{
	int rv;
	struct io_cmd_ctx *cmd_ctx = arg;
	struct nvmeibs_nr_cmd *cmd;
	const struct volume_client_io_req_base *io_req = wire_buf;
	struct nvmeibs_client *cl = NR2C(cmd_ctx->nrch);

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	/* nvme-req */
	cmd = &cmd_ctx->nrch->io_cmds[cmd_ctx->idx];
	cmd->ind_op = io_req->ind_op;
	if ((cmd->req.nvme_op = ind_2_nvme(cmd->ind_op)) < 0) {
		_NE(error_2_nordda_io_cmd_process, "Unknown op, send err rsp");
		rv = NVMEIBS_IO_RSP_ERR_INV_OP;
		goto out;
	}
	cmd->req.buf_offset = 0;
	cmd->req.disk_block = be64_to_cpu(io_req->sw_slba);
	cmd->req.data_len = be64_to_cpu(io_req->data_len);
	cmd->req.cb = io_cmd_cb;
	cmd->req.qid_hint_plus1 = cmd_ctx->nrch->net->params.ch_index + 1;
	if (cmd_ctx->di->mtdt_extd) {
		cmd->req.metadata = NULL;
		cmd->req.mtdt_size = 0;
	}
	else {
		cmd->req.metadata = cmd->md.virt;
		cmd->req.mtdt_size = be32_to_cpu(io_req->md_desc.size);
	}

	if (nvmeibs_disk_collect_stats && cmd->ind_op == NVMEIB_IND_OP_IO_DSM) {
		/* [NVMESH-2935]: Calculate the total LBAs of all nvmeib_dsm_cmd */
		struct nvmeib_dsm_range dsm_range;
		unsigned i;

		/* Copy the dsm-cmds 1-by-1 onto the stack and count the number of LBAs */
		cmd->req.n_dsm_lba = 0;
		for (i = 0; i < cmd->req.data_len / sizeof(dsm_range); i++) {
			size_t sz = sizeof(dsm_range);
			off_t off  = i * sizeof(dsm_range);
			nvmeib_mem_sync_map_for_cpu(cmd->bb_map, off, sz);
			sg_copy_buffer(cmd->bb_map->mem_table.sgl, cmd->bb_map->mem_table.nents,
				       &dsm_range, sz, off, true);
			cmd->req.n_dsm_lba += le32_to_cpu(dsm_range.nlb);
		}
	}

	/* journal MD piggyback */
	if (io_req->jmd_pb[0]) {
		if (1) {
			_NE(error_nordda_io_cmd_process_base_jmd_pb, "Obsolete mode");
			rv = NVMEIBS_IO_RSP_ERR_JMD_PB;
			goto out;
		}
		if (cmd_ctx->di->mtdt_extd) {
			/* Copy from BB into MD buffer */
			struct nvmeib_buffer bb_buf;
			size_t io_len = be64_to_cpu(io_req->data_len);
			int sw_sector_shift, md_sw_entry_len;

			get_md_sw_params(cmd_ctx->di, io_req, &sw_sector_shift, &md_sw_entry_len);
			nvmeib_buffer_init_sgl(&bb_buf, cmd->bb_map->mem_table.sgl,
					       cmd->bb_map->mem_table.nents, io_len, 0);
			nvmeib_buffer_copy_md_ext_to_buffer(&bb_buf, 1 << sw_sector_shift, md_sw_entry_len,
						     cmd->md.virt, cmd->md.size);
		}

		rv = io_req_jmd_piggyback(cmd_ctx->nrch, cl, io_req, cmd->md.virt, 0);
		if (rv) {
			rv = NVMEIBS_IO_RSP_ERR_JMD_PB;
			goto out;
		}
	}
#ifdef NVMEIB_POISON_DD_NR_BB
	if (io_req->ind_op == NVMEIB_IND_OP_IO_READ) {
		struct nvmeib_buffer bb_buf;
		u32 pattern = 0xdeadbeef;

		nvmeib_buffer_init_sgl(&bb_buf, cmd->bb_map->mem_table.sgl,
					cmd->bb_map->mem_table.nents,
					cmd->bb_map->n_pages << PAGE_SHIFT, 0);
		nvmeib_buffer_fill(&bb_buf, &pattern, sizeof(pattern));
	}
#endif

	if (io_req->ind_op == NVMEIB_IND_OP_IO_WRITE)
		_ND(trace_nordda_io_cmd_process_base, "write io_cmd @CMD_PTR: disk=@DISK_STR, op=@NVME_OP, prpl_phys=@PRPL_PHYS, buf_offset=@BUF_OFFSET, "
			"disk_block=@DISK_BLOCK, len=@LEN_LONG md=@MD_PTR",
			cmd, cmd_ctx->di->disk_id, cmd->req.nvme_op, cmd->req.prpl_phys,
			cmd->req.buf_offset, (unsigned long)cmd->req.disk_block, cmd->req.data_len,
			cmd->md.virt);
	else
		_ND(trace_1_nordda_io_cmd_process_base, "io_cmd @CMD_PTR: disk=@DISK_STR, op=@NVME_OP, prpl_phys=@PRPL_PHYS, buf_offset=@BUF_OFFSET, "
			"disk_block=@DISK_BLOCK, data_len=@DATA_LEN_LONG",
			cmd, cmd_ctx->di->disk_id, cmd->req.nvme_op, cmd->req.prpl_phys,
			cmd->req.buf_offset, (unsigned long)cmd->req.disk_block, cmd->req.data_len);

	cmd_ctx->cmd = cmd;
	rv = sizeof(*io_req);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_req_srv_ext1_decode)
{
	struct io_cmd_ctx *cmd_ctx = arg;
	struct nvmeibs_nr_cmd *cmd = cmd_ctx->cmd;
	const struct volume_client_io_req_ext1 *io_req = wire_buf;
	struct nvmeibs_disk_info *di = cmd_ctx->nrch->rionic->lionic->disk->di;
	int rv;

	BUG_ON(!cmd);

	if (!wire_buf) {
		rv = 0;
	} else {
		if (io_req->is_sub_block) {
			BUG_ON(io_req->sub_block_idx > 7);
			cmd->req.use_hw_blocks = 1;
			cmd->req.disk_block = nvmeib_translate_sw_addr_to_subblock_addr(
				cmd->req.disk_block,
				NVMEIBC_SECTOR_SHIFT,
				di->block_shift,
				io_req->sub_block_idx
			);
		}
		else {
			cmd->req.use_hw_blocks = 0;
		}
		rv = sizeof(*io_req);
	}

	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_io_req_srv_ext2_decode)
{
	struct io_cmd_ctx *cmd_ctx = arg;
	struct nvmeibs_nr_cmd *cmd = cmd_ctx->cmd;
	const struct volume_client_io_req_ext2 *io_req = wire_buf;
	int rv;
	
	BUG_ON(!cmd);
	
	if (!wire_buf) {
		cmd->req.stats.is_recovery = false;
		rv = 0;
	} else {
		cmd->req.stats.is_recovery = !!io_req->is_recovery;
		rv = sizeof(*io_req);
	}
	
	return rv;
}

static void _io_cmd_process(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct volume_client_req *req;
	struct volume_server_io_rsp_base io_rsp = {};
	struct io_cmd_ctx cmd_ctx = {};
	/* TBD: Allocate size based on vex_size */
	void *payload = &io_rsp;
	size_t payload_len = sizeof(io_rsp);
	int rv = 0;
	u64 req_tag;
	u16 req_version;
	u32 ch_version;
	struct nvmeib_iu *send_ioctx;
	void *end_copy_req;

	__NFIN;

	io_rsp.piggyback_read = NVMEIBS_NORDDA_PIGGYBACK_READ_POISON;
	BUG_ON(cl->vex_io_rsp_ops->vex_ext != vex_base);
	req = recv_ioctx->buf;
	if (be16_to_cpu(req->version_tag) != cl->vex_io_req_ops->vex_ext) {
		_NT(_io_cmd_process_t1, "Invalid version tag @UINT for NVMEIBC_IO_CMD", be16_to_cpu(req->version_tag));
		rv = NVMEIBS_IO_RSP_ERR_INV_VER_TAG;
		goto send_err_resp;
	}

	cmd_ctx.nrch = nrch;
	cmd_ctx.recv_ioctx = recv_ioctx;
	cmd_ctx.di = cl->di;
	req_tag = be64_to_cpu(req->hdr.tag);
	cmd_ctx.idx = nordda_tag_decode_index(req_tag);
	req_version = nordda_tag_decode_version(req_tag);
	ch_version = nordda_tag_decode_ch_version(req_tag);
	if (cmd_ctx.idx >= cl->nrch_ioreq_num) {
		_NE(error_1_nordda_io_cmd_process, "Invalid request index @IDX_LLONG (max supported index is @NRCH_IOREQ_NUM)",
			cmd_ctx.idx, cl->nrch_ioreq_num);
		rv = NVMEIBS_IO_RSP_ERR_INV_TAG;
		goto send_err_resp;
	}

	_ND(trace_nordda_io_cmd_process, "--- Start handling NVMEIBC_IO_CMD message, nrch @NRCH_NAME (@CH_PTR),"
			" req_idx @REQ_INDEX, req_version @VERSION",
	 nrch->name, nrch, cmd_ctx.idx, req_version);

	rv = CALL_VEX_OP(decode,
				vex_nrch_io_req_srv_ops, TWO_EXT,
					base, vex_nrch_io_req_srv_base_decode,
					ext1, vex_nrch_io_req_srv_ext1_decode,
					ext2, vex_nrch_io_req_srv_ext2_decode,
						cl->vex_io_req_ops, NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req),
						recv_ioctx->buf + recv_ioctx->size, &cmd_ctx);
	if (rv < 0) {
		goto send_err_resp;
	}

	cmd_ctx.cmd->gen_cmd = false;
	cmd_ctx.cmd->version = req_version;
	cmd_ctx.cmd->ch_version = ch_version;
	cmd_ctx.cmd->req_hdr_tag = req->hdr.tag;
	cmd_ctx.cmd->req_version_tag = req->version_tag;
	cmd_stats_md_on_cmd_process(cmd_ctx.cmd);

	/* Copy io_req part of recv req into cmd buffer
	 * TBD: Decode into a host struct instead
	 */
	end_copy_req = (void *)min_t(unsigned long,
		(unsigned long)recv_ioctx->buf + recv_ioctx->recv_size,
		(unsigned long)NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req) + sizeof(struct volume_client_io_req_max_DONT_USE));
	cmd_ctx.cmd->cmd_req_sz = end_copy_req - NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req);
	memcpy(cmd_ctx.cmd->cmd_req,
	       NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(req),
	       cmd_ctx.cmd->cmd_req_sz);

	nvmeibs_nr_lat_meas_record_rx_md(nrch, recv_ioctx);

#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
	atomic64_inc(&cmd_ctx.cmd->n_submit);
#endif

#ifdef NVMEIB_TRANSPORT_SKIP_STAGES
	if (unlikely(nvmeibs_nr_skip_disk_access)) {
		cmd_ctx.cmd->req.skipped = true;
		if ((P2NV(nrch->net->params.port)->dev_type == DT_siw && in_interrupt())) {
			/* For SIW, if notify_on_wq is not set, then we need to change context or we will cause a deadlock */
			WQ_INIT_WORK(&cmd_ctx.cmd->work, io_cmd_cb_work);
			nvmeibs_nordda_add_work(nrch, &cmd_ctx.cmd->work);
		} else {
			io_cmd_cb(cmd_ctx.cmd, 0, 0);
		}
		post_recv_iu(nrch, recv_ioctx);
		goto out;
	}
#endif
	if (nvmeibs_nr_post_recv_on_send_comp) {
		cmd_ctx.cmd->recv_ioctx = recv_ioctx;
		recv_ioctx = NULL;
	}

	if ((rv = submit_local_cmd(cmd_ctx.di, &cmd_ctx.cmd->req)) < 0) {
		_ND(trace_1_nordda_io_cmd_process, "Fail to submit cmd to disk (rv = @RV)", rv);
#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
		atomic64_dec(&cmd_ctx.cmd->n_submit);
#endif
		rv = NVMEIBS_IO_RSP_ERR_SUBMIT;
		if (nvmeibs_nr_post_recv_on_send_comp) {
			recv_ioctx = cmd_ctx.cmd->recv_ioctx;
			cmd_ctx.cmd->recv_ioctx = NULL;
		}
		goto send_err_resp;
	}

	nvmeibs_nr_lat_meas_record_io_submit(nrch, cmd_ctx.cmd);

	post_recv_iu(nrch, recv_ioctx);
	goto out;

send_err_resp:
	// we reach here when processing failed and we need to reply with an error

	//TBD: Call into vex encode
	io_rsp.comp_code = cpu_to_be32(rv);
	cmd_ended(nrch, req->hdr.tag);
	if ((send_ioctx = nordda_send_ioctx_get_or_disconnect(nrch->net))) {
		nrch_send_rsp_and_post_recv(nrch,
			rv ? NVMEIBS_RSP_IO_OPCODE_ERR : NVMEIBS_RSP_IO_OPCODE_OK,
			req->hdr.tag, req->version_tag, send_ioctx, payload, payload_len,
			NVMEIB_REPLY_NO_IO, cmd_ctx.cmd->version, recv_ioctx);
		_ND(trace_2_nordda_io_cmd_process, "--- Finish handling (@RV) NVMEIBC_IO_CMD message, nrch @NRCH_NAME (@CH_PTR),"
				" req_idx @REQ_INDEX, req_version @VERSION",
			rv, nrch->name, nrch, cmd_ctx.idx, cmd_ctx.cmd ? cmd_ctx.cmd->version : 0);
	} else {
		post_recv_iu(nrch, recv_ioctx);
	}

out:
	__NFOUT;
}

/* Let Serjio fill UUID's jmdc and its desc to @cmd's first BB and
   @rsp, respectively. Then RDMA write it to client's sink buffer.
   The caller will send the rsp */
static inline int handle_get_uuid_jour(struct nvmeibs_nr_channel *nrch,
	struct nvmeibs_nr_cmd *cmd,
	struct nvmeib_gen_cmd_param *gen_param,
	union nvmeib_gen_cmd_rsp *rsp)
{
	size_t bb_size = (nrch->io_cmd_n_pages << PAGE_SHIFT);
	struct handle_get_uuid_jour_cb_param param = {
		.gen_param = gen_param,
		.gen_rsp = rsp,
	};
	int n_jmdc_bufs, n_ent_md_bufs;
	int rv = 0;
	enum {
		UUID_JOUR_RDMA_RSP_JMDC = 0,
		UUID_JOUR_RDMA_RSP_ENT_MD,
		UUID_JOUR_RDMA_RSP_MAX,
	};
	struct nvmeib_send_wr rdma_wr[UUID_JOUR_RDMA_RSP_MAX];
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	struct ib_sge rdma_sge[UUID_JOUR_RDMA_RSP_MAX];
	int send_flags = 0;

	NFIN;
	n_jmdc_bufs = DIV_ROUND_UP(gen_param->uj.jmdc_dest.remote.len, PAGE_SIZE);
	n_ent_md_bufs = DIV_ROUND_UP(gen_param->uj.ent_md_dest.remote.len, PAGE_SIZE);
	if (n_jmdc_bufs + n_ent_md_bufs > cmd->bb_map->n_pages) {
		_NE(error_nvmeibs_nordda_c_1155, "Bounce-Buffer Size (@SIZE_T) is too small for JMDC Size (@REMOTE_LEN) and Entry MD Size (@REMOTE_LEN)", bb_size, gen_param->uj.jmdc_dest.remote.len, gen_param->uj.ent_md_dest.remote.len);
		rv = -ENOMEM;
		goto out;
	}
	nvmeib_buffer_init_sgl(&gen_param->uj.jmdc_dest.local,
			       cmd->bb_map->mem_table.sgl,
			       cmd->bb_map->mem_table.nents,
				cmd->bb_map->n_pages << PAGE_SHIFT, 0);
	nvmeib_buffer_init_sgl(&gen_param->uj.ent_md_dest.local,
				cmd->bb_map->mem_table.sgl,
				cmd->bb_map->mem_table.nents,
				cmd->bb_map->n_pages << PAGE_SHIFT, n_jmdc_bufs << PAGE_SHIFT);

	if ((rv = nvmeibs_gen_cmd_handle_get_uuid_jour(NR2C(nrch)->di, &param)))
		goto out;

	if (!param.jmdc_sz && !param.ent_md_sz)
		goto out;

	/* It's faster to just sync the whole BB (it's only 32 pages) */
	nvmeib_mem_sync_map_for_cpu(cmd->bb_map, 0, cmd->io_cmd_mem.size);

#if ENABLE_SIW
	if (P2NV(cmd->nrch->net->params.port)->dev_type == DT_siw)
		send_flags |= SIW_IB_SEND_MORE_WQES | SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	/* WR for JMDC */
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).wr_id =
		nvmeib_encode_wr_id(NVMEIB_WR_GEN_UUID_RSP_JMDC, cmd->io_iu->index);
	nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).remote_addr = gen_param->uj.jmdc_dest.remote.raddr;
	nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).rkey = gen_param->uj.jmdc_dest.remote.rkey;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).num_sge = 1;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).sg_list = &rdma_sge[UUID_JOUR_RDMA_RSP_JMDC];
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).send_flags = send_flags;
	nvmeib_send_wr_set_next(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC], &rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]);

	rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].lkey = cmd->bb_map->lkey;
	rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].addr = cmd->bb_map->ioaddr;
	rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].length = param.jmdc_sz;
	
	_NT(trace_handle_get_uuid_jour_jmdc_rdma, 
		"RDMA_WR: @LKEY:@IOADDR:@LENGTH_INT-> @RKEY:@IOADDR",
		rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].lkey,
		rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].addr,
		rdma_sge[UUID_JOUR_RDMA_RSP_JMDC].length,
		nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).rkey,
		nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]).remote_addr);

	/* WR for Ent MD */
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).wr_id =
		nvmeib_encode_wr_id(NVMEIB_WR_GEN_UUID_RSP_ENT_MD, cmd->io_iu->index);
	nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).remote_addr = gen_param->uj.ent_md_dest.remote.raddr;
	nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).rkey = gen_param->uj.ent_md_dest.remote.rkey;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).num_sge = 1;
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).sg_list = &rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD];
	nvmeib_send_wr_common(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).send_flags = send_flags;
	nvmeib_send_wr_clear_next(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]);

	rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].lkey = cmd->bb_map->lkey;
	rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].addr = cmd->bb_map->ioaddr + gen_param->uj.jmdc_dest.remote.len;
	rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].length = param.ent_md_sz;
	
	_NT(trace_handle_get_uuid_jour_ent_md_rdma, 
		"RDMA_WR: @LKEY:@IOADDR:@LENGTH_INT-> @RKEY:@IOADDR",
		rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].lkey,
		rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].addr,
		rdma_sge[UUID_JOUR_RDMA_RSP_ENT_MD].length,
		nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).rkey,
		nvmeib_send_wr_rdma(rdma_wr[UUID_JOUR_RDMA_RSP_ENT_MD]).remote_addr);

	if ((rv = ib_post_send(cmd->nrch->net->qp,
			nvmeib_send_wr_to_ib_ptr(rdma_wr[UUID_JOUR_RDMA_RSP_JMDC]), &bad_wr)) < 0) {
		_NT(trace_s_nordda_handle_get_uuid_jour_rdma_wr_fail,
		    "ib_post_send failed (@RV) for GET_UUID_JOUR RDMA Response", rv);
		rv = NVMEIBS_IO_RSP_ERR_UUID_JOUR;
		goto out;
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

struct gens_cmd_ctx {
	struct nvmeibs_nr_cmd *cmd;
	struct nvmeib_gen_cmd_param gen_req;
	union nvmeib_gen_cmd_rsp gen_rsp;
};

/* WAS: handle_blkset_recovered_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_br_req_srv_base_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_blkset_recovered_base *req = wire_buf;
	struct nvmeib_gen_cmd_param *gen_param = &cmd_ctx->gen_req;

	NFIN;
	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*req) > wire_buf_end);

	gen_param->br.uuid = req->uuid;
	strlcpy(gen_param->br.ds_uuid, req->ds_uuid, NVMEIB_GID_STR_MAX);
	gen_param->br.lock_ent = be64_to_cpu(req->lock_ent);
	gen_param->br.blkset_num = be64_to_cpu(req->blkset_num);
	gen_param->br.blkset_slba = be64_to_cpu(req->blkset_slba);
	gen_param->br.rng_id = be32_to_cpu(req->rng_id);
	gen_param->br.ent_id = be32_to_cpu(req->ent_id);
	gen_param->br.pass2toma = req->pass2toma;
	gen_param->br.gen_id = be64_to_cpu(req->gen_id);

	return sizeof(*req);
}




VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_br_rsp_srv_base_encode)
{
	struct volume_server_gen_rsp_blkset_recovered_base *wire_lock_rsp = wire_buf;
	struct gens_cmd_ctx *cmd_ctx = arg;
	union nvmeib_gen_cmd_rsp *gen_rsp = &cmd_ctx->gen_rsp;

	BUG_ON(!wire_lock_rsp);
	BUG_ON(wire_buf + sizeof(*wire_lock_rsp) > wire_buf_end);

	wire_lock_rsp->uuid = gen_rsp->br.uuid;
	wire_lock_rsp->rng_id = cpu_to_be32(gen_rsp->br.rng_id);
	wire_lock_rsp->ent_id = cpu_to_be32(gen_rsp->br.ent_id);
	wire_lock_rsp->status = cpu_to_be32(gen_rsp->br.status);

	return sizeof(*wire_lock_rsp);
}

static int handle_get_ec_db(struct nvmeibs_nr_channel *nrch,
	struct nvmeibs_nr_cmd *cmd,
	const struct nvmeib_gen_cmd_param *gen_param,
	union nvmeib_gen_cmd_rsp *gen_rsp, size_t *rdma_len)
{
	dma_addr_t dma_addr = cmd->bb_map->ioaddr;
	const struct nvmeib_gen_cmd_data *sink = &gen_param->db.db_dest;
	int buf_len = sink->remote.len;
	struct ib_sge list;
	struct nvmeib_send_wr wr = {};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	struct nvmeibs_disk_info *di;
	struct nvmeib_buffer bb_buf = {};

	NFIN;
	di = NR2C(nrch)->di;
	if (!di) {
		_NE(error_nordda_handle_get_ec_db, "disk info not set");
		rv = NVMEIBS_IO_RSP_ERR_GET_EC_DB;
		goto out;
	}
	if (buf_len > cmd->bb_map->n_pages * PAGE_SIZE) {
		_NT(trace_nordda_handle_get_ec_db, "adjust the reply size  @BUF_LEN", buf_len);
		buf_len = cmd->bb_map->n_pages * PAGE_SIZE;
	}
	_NT(trace_1_nordda_handle_get_ec_db, "rkey=@RKEY  raddr=@RADDR  dbits=@DBITS  stales=@STALES  full_val=@FULL_VAL",
	   sink->remote.rkey, sink->remote.raddr, gen_param->db.get_dbits, gen_param->db.get_stales,
	   gen_param->db.get_full_val);

	/* JH IOMMU: Added. Return ownership of BB to CPU */
	nvmeib_mem_sync_map_for_cpu(cmd->bb_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	nvmeib_buffer_init_sgl(&bb_buf, cmd->bb_map->mem_table.sgl,
				cmd->bb_map->mem_table.nents,
				cmd->bb_map->n_pages << PAGE_SHIFT, 0);

	rv = nvmeibs_disk_locks_get_ec_dirty_bytes(di->priv, 0,
				gen_param->db.lba, gen_param->db.n_lba, &bb_buf,
				gen_param->db.get_dbits, gen_param->db.get_stales, gen_param->db.get_full_val, gen_param->db.reserved,
				&gen_rsp->db.nlba);
	if (rv) {
		_NE(error_1_nordda_handle_get_ec_db, "Fail to fetch dirty bits");
		rv = NVMEIBS_IO_RSP_ERR_GET_EC_DB;
		goto out;
	}

	/* JH IOMMU: Return ownership to NIC */
	nvmeib_mem_sync_map_for_device(cmd->bb_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	list.addr = dma_addr;
	list.length = buf_len;
	list.lkey = cmd->bb_map->lkey;
	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).wr_id =
		nvmeib_encode_wr_id(NVMEIB_RDMA_LAST, cmd->io_iu->index);
	nvmeib_send_wr_rdma(wr).remote_addr = sink->remote.raddr;
	nvmeib_send_wr_rdma(wr).rkey = sink->remote.rkey;
	nvmeib_send_wr_common(wr).num_sge = 1;
	nvmeib_send_wr_common(wr).sg_list = &list;

#if ENABLE_SIW
	if (P2NV(nrch->net->params.port)->dev_type == DT_siw)
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_MORE_WQES | SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	rv = nvmeibs_ib_post_send(nrch->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);

#if ENABLE_SIW
	if (P2NV(cmd->nrch->net->params.port)->dev_type == DT_siw)
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_MORE_WQES | SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	if (rv) {
		_NT(trace_2_nordda_handle_get_ec_db, "Fail to send ec dirty bits");
		rv = NVMEIBS_IO_RSP_ERR_RDMA;
	}

	*rdma_len = buf_len;

out:
	NFOUT;
	return rv;
}

/* WAS: gens_op_get_uuid_jour_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_uj_req_srv_base_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_uuid_jour_base *uuid_jour_req = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(uuid_jour_req + 1) > wire_buf_end);

	cmd_ctx->gen_req.uj.client_uuid = uuid_jour_req->client_uuid;
	memcpy(cmd_ctx->gen_req.uj.sgmnt_uuid, uuid_jour_req->sgmnt_uuid, NVMEIB_GID_STR_MAX);
	cmd_ctx->gen_req.uj.jmdc_dest.remote.raddr = be64_to_cpu(uuid_jour_req->jmdc_rai.raddr);
	cmd_ctx->gen_req.uj.jmdc_dest.remote.rkey = be32_to_cpu(uuid_jour_req->jmdc_rai.rkey);
	cmd_ctx->gen_req.uj.jmdc_dest.remote.len = be32_to_cpu(uuid_jour_req->jmdc_rai.len);
	cmd_ctx->gen_req.uj.ent_md_dest.remote.raddr = be64_to_cpu(uuid_jour_req->ent_md_rai.raddr);
	cmd_ctx->gen_req.uj.ent_md_dest.remote.rkey = be32_to_cpu(uuid_jour_req->ent_md_rai.rkey);
	cmd_ctx->gen_req.uj.ent_md_dest.remote.len = be32_to_cpu(uuid_jour_req->ent_md_rai.len);

	return sizeof(*uuid_jour_req);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_uj_req_srv_ext1_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_uuid_jour_ext1 *uuid_jour_req = wire_buf;
	int rv;

	if (!wire_buf) {
		cmd_ctx->gen_req.uj.binje = NVMEIB_EC_INVALID_JOURNAL_BINJE; /* No multi slice */
		rv = 0;
	} else {
		BUG_ON(wire_buf + sizeof(uuid_jour_req) > wire_buf_end);
		cmd_ctx->gen_req.uj.binje = be32_to_cpu(uuid_jour_req->binje);
		rv = sizeof(*uuid_jour_req);
	}

	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_uj_rsp_srv_base_encode)
{
	struct volume_server_gen_rsp_uuid_jour_base *wire_lock_rsp = wire_buf;
	struct gens_cmd_ctx *cmd_ctx = arg;
	union nvmeib_gen_cmd_rsp *gen_rsp = &cmd_ctx->gen_rsp;

	BUG_ON(!wire_lock_rsp);
	BUG_ON(wire_buf + sizeof(*wire_lock_rsp) > wire_buf_end);

	wire_lock_rsp->uuid = gen_rsp->uj.jour.client_uuid;
	wire_lock_rsp->rng_id = cpu_to_be32(gen_rsp->uj.jour.rng_id);
	wire_lock_rsp->rng_slba = cpu_to_be64(gen_rsp->uj.jour.rng_slba);
	wire_lock_rsp->rng_nlba = cpu_to_be32(gen_rsp->uj.jour.rng_nlba);
	wire_lock_rsp->rng_gen_id = cpu_to_be64(gen_rsp->uj.jour.rng_gen_id);
	wire_lock_rsp->jmdc_len = cpu_to_be32(gen_rsp->uj.jmdc_len);
	wire_lock_rsp->ent_md_len = cpu_to_be32(gen_rsp->uj.ent_md_len);

	memcpy(wire_lock_rsp->serjio_boot_id, gen_rsp->uj.jour.serjio_boot_id, NVMEIB_GID_STR_MAX);

	nvmeib_bitmap_to_be32(wire_lock_rsp->dirty_ents_bitmap, gen_rsp->uj.jour.dirty_ents_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	nvmeib_bitmap_to_be32(wire_lock_rsp->abnd_ents_bitmap, gen_rsp->uj.jour.abnd_ents_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);

	return sizeof(*wire_lock_rsp);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_uj_rsp_srv_ext1_encode)
{
	struct volume_server_gen_rsp_uuid_jour_ext1 *wire_gen_rsp = wire_buf;
	struct gens_cmd_ctx *cmd_ctx = arg;
	union nvmeib_gen_cmd_rsp *gen_rsp = &cmd_ctx->gen_rsp;

	BUG_ON(!wire_gen_rsp);
	BUG_ON(wire_buf + sizeof(*wire_gen_rsp) > wire_buf_end);

	wire_gen_rsp->binje = cpu_to_be32(gen_rsp->uj.jour.binje);

	return sizeof(*wire_gen_rsp);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_uj_rsp_srv_ext2_encode)
{
	struct volume_server_gen_rsp_uuid_jour_ext2 *wire_gen_rsp = wire_buf;
	struct gens_cmd_ctx *cmd_ctx = arg;
	union nvmeib_gen_cmd_rsp *gen_rsp = &cmd_ctx->gen_rsp;

	BUG_ON(!wire_gen_rsp);
	BUG_ON(wire_buf + sizeof(*wire_gen_rsp) > wire_buf_end);

	wire_gen_rsp->rng_blk = cpu_to_be32(gen_rsp->uj.jour.rng_blk);
	wire_gen_rsp->n_ents = cpu_to_be32(gen_rsp->uj.jour.n_ents);

	return sizeof(*wire_gen_rsp);
}

/* WAS: gens_op_get_ec_db_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_db_req_srv_base_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct disk_req_get_ec_dirty_bits_base *get_ec_db_req = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(get_ec_db_req + 1) > wire_buf_end);

	cmd_ctx->gen_req.db.lba = be64_to_cpu(get_ec_db_req->lba);
	cmd_ctx->gen_req.db.n_lba = be32_to_cpu(get_ec_db_req->sectors);
	cmd_ctx->gen_req.db.get_dbits = get_ec_db_req->get_dbits;
	cmd_ctx->gen_req.db.get_stales = get_ec_db_req->get_stales;
	cmd_ctx->gen_req.db.get_full_val = get_ec_db_req->get_full_val;
	cmd_ctx->gen_req.db.db_dest.remote.rkey = be32_to_cpu(get_ec_db_req->rai.rkey);
	cmd_ctx->gen_req.db.db_dest.remote.raddr = be64_to_cpu(get_ec_db_req->rai.raddr);
	cmd_ctx->gen_req.db.db_dest.remote.len = be32_to_cpu(get_ec_db_req->rai.len);

	return sizeof(*get_ec_db_req);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_nrch_gen_db_rsp_srv_base_encode)
{
	struct volume_server_gen_rsp_get_dirty_bits_base *wire_lock_rsp = wire_buf;
	struct gens_cmd_ctx *cmd_ctx = arg;
	union nvmeib_gen_cmd_rsp *gen_rsp = &cmd_ctx->gen_rsp;

	BUG_ON(!wire_lock_rsp);
	BUG_ON(wire_buf + sizeof(*wire_lock_rsp) > wire_buf_end);

	wire_lock_rsp->nlbas = cpu_to_be32(gen_rsp->db.nlba);

	return sizeof(*wire_lock_rsp);
}

/* WAS: gens_op_free_jrnl_ents_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_fje_srv_base_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_free_ents_base *req = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(req + 1) > wire_buf_end);

	_NT(trace_vex_nrch_gen_fje_srv_base_decode,
		"Handling Free Journal Entries for Seg: @SEG_UUID_STR from @NVMEIB_RECOV_SRC_STR -"
		" Num Ents: @N_ENTS Blkset LBA: @BLKSET_SLBA Pass2Toma: @BOOL",
		req->seg_uuid, nvmeib_recov_src_str(req->src), be16_to_cpu(req->num_ents),
		be64_to_cpu(req->blkset_slba), !!req->pass2toma);


	strlcpy(cmd_ctx->gen_req.free_ents.serjio_boot_id, req->serjio_boot_id, NVMEIB_GID_STR_MAX);
	strlcpy(cmd_ctx->gen_req.free_ents.seg_uuid, req->seg_uuid, NVMEIB_GID_STR_MAX);
	cmd_ctx->gen_req.free_ents.src = req->src;
	cmd_ctx->gen_req.free_ents.num_ents = be16_to_cpu(req->num_ents);
	cmd_ctx->gen_req.free_ents.pass2toma = req->pass2toma;
	cmd_ctx->gen_req.free_ents.blkset_num = be64_to_cpu(req->blkset_num);
	cmd_ctx->gen_req.free_ents.blkset_slba = be64_to_cpu(req->blkset_slba);
	cmd_ctx->gen_req.free_ents.lock_ent = be64_to_cpu(req->lock_ent);

	return sizeof(*req);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_fje_ent_srv_base_decode)
{
	struct nvmeib_free_ents_data *ents_data = arg;
	const struct wire_free_ents_entry_base *base = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	ents_data->rng_idx = be16_to_cpu(base->rng_idx);
	ents_data->rng_gen_id = be64_to_cpu(base->rng_gen_id);
	ents_data->ent_idx = be16_to_cpu(base->ent_idx);
	ents_data->ent_md.ent_gen_id = base->ent_md_gen_id;

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_fje_ent_srv_ext1_decode)
{
	struct nvmeib_free_ents_data *ents_data = arg;
	const struct wire_free_ents_entry_ext1 *ext1 = wire_buf;

	if (!wire_buf) {
		/* Defaults */
		ents_data->rng_binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	ents_data->rng_binje = binje_to_be(ext1->binje);

	return sizeof(*ext1);
}

/* WAS: gens_op_jentry_erase_base */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_je_srv_base_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_jentry_erase_base *base = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(base + 1) > wire_buf_end);
	cmd_ctx->gen_req.je.rng_gen_id = be64_to_cpu(base->rng_gen_id);
	cmd_ctx->gen_req.je.rng_idx = be32_to_cpu(base->rng_id);
	cmd_ctx->gen_req.je.ent_erase.ent_idx = be32_to_cpu(base->ent_id);
	cmd_ctx->gen_req.je.ent_erase.ent_md = base->ent_md;
	cmd_ctx->gen_req.je.ent_erase.ent_swlba = be64_to_cpu(base->sw_jlba);

	if (ops->vex_ext == link_ext)
		_NT(trace_vex_je_decode_base, "Decode NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP)  - Range: @JRNL_RNG_IDX Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, LBA: @SW_LBA", NVMEIB_GEN_OP_JENTRY_ERASE,
			cmd_ctx->gen_req.je.rng_idx, cmd_ctx->gen_req.je.ent_erase.ent_idx,
		 cmd_ctx->gen_req.je.rng_gen_id, cmd_ctx->gen_req.je.ent_erase.ent_idx,
		 cmd_ctx->gen_req.je.ent_erase.ent_swlba);

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_nrch_gen_je_srv_ext1_decode)
{
	struct gens_cmd_ctx *cmd_ctx = arg;
	const struct volume_client_gen_req_jentry_erase_ext1 *ext1 = wire_buf;

	if (!wire_buf) {
		/* Defaults */
		cmd_ctx->gen_req.je.rng_binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		return 0;
	}

	BUG_ON((const void *)(ext1 + 1) > wire_buf_end);
	cmd_ctx->gen_req.je.rng_binje = binje_from_be(ext1->rng_binje);

	if (ops->vex_ext == link_ext)
		_NT(trace_vex_je_decode_ext1, "Decode NVMEIB_GEN_OP_JENTRY_ERASE (@GEN_CMD_OP)  - Range: @JRNL_RNG_IDX N: @BINJE Entry: @JRNL_RNG_ENT_IDX GenID: @JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID, LBA: @SW_LBA", NVMEIB_GEN_OP_JENTRY_ERASE,
			cmd_ctx->gen_req.je.rng_idx, cmd_ctx->gen_req.je.rng_binje,
			cmd_ctx->gen_req.je.ent_erase.ent_idx,
			cmd_ctx->gen_req.je.rng_gen_id, cmd_ctx->gen_req.je.ent_erase.ent_idx,
			cmd_ctx->gen_req.je.ent_erase.ent_swlba);

	return sizeof(*ext1);
}

static void __complete_send_nordda_cmd(struct nvmeibs_nr_cmd *cmd, struct nvmeib_iu *send_ioctx, struct volume_server_gen_rsp *wire_gen_rsp,
				       enum nvmeib_wr_opcode send_wr_opcode, size_t wire_gen_rsp_len, int rv, int rsp_opcode)
{
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	cmd_ended(nrch, cmd->req_hdr_tag);
	wire_gen_rsp->comp_code = cpu_to_be32(rv);
	if (send_wr_opcode == NVMEIB_REPLY_GEN_W_RDMA) {
		WARN_ON(send_ioctx != cmd->io_iu);
		atomic_inc(&cmd->n_send_rsp_io);
	} else {
		WARN_ON(send_ioctx);
		send_ioctx = nordda_send_ioctx_get_or_disconnect(nrch->net);
	}

	if (send_ioctx) {
		_ND(trace_nordda___complete_send_nordda_cmd, "--- Response (@RV) to GEN_CMD @GEN_OP_STR (@OP) message,"
						" nrch @NRCH_NAME (@CH_PTR), req_index @REQ_INDEX, req_version @VERSION, req tag @TAG, iu index @INDEX",
						rsp_opcode, nvmeib_gen_op_str(cmd->gen_op), cmd->gen_op,
						nrch->name, nrch, cmd->idx, cmd->version, cmd->req_hdr_tag, send_ioctx->index);
		/* Includes just the RDMA Write Length so far, 
		 * the size of the RDMA Send will be added in nvmeibs_client_send_msg */
		send_ioctx->send_size = CMD_STATS_MD_HEAD(cmd).send_rsp_len;
		/* Add the length of the RDMA Send */
		CMD_STATS_MD_HEAD(cmd).send_rsp_len += offsetof(struct volume_server_rsp, payload) + wire_gen_rsp_len;
		cmd_stats_md_on_send_rsp(cmd);
		nrch_send_rsp(NR2C(nrch), nrch, rsp_opcode,
			cmd->req_hdr_tag, cmd->req_version_tag, send_ioctx, wire_gen_rsp,
			wire_gen_rsp_len, send_wr_opcode, cmd->version);
	}
}

void nvmeibs_async_cookie_nordda_completion(
    union nvmeibs_async_cookie_ctx *ctx, int rv)
{
	struct gens_cmd_ctx *cmd_ctx = ctx->nordda.cmd_ctx;
	struct nvmeibs_nr_cmd *cmd = cmd_ctx->cmd;
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct volume_server_gen_rsp wire_gen_rsp;
	size_t wire_gen_rsp_len = offsetof(typeof(wire_gen_rsp), payload);
	const struct vex_ops *br_rsp_vex_ops = vex_select_ops(cl->link_version, vex_nrch_gen_br_rsp_srv_ops);

	cmd_ctx->gen_rsp.br.status = rv;

	if (rv) goto send_rsp;

	if ((rv = CALL_VEX_OP(encode,
		vex_nrch_gen_br_rsp_srv_ops, BASE_ONLY,
		base, vex_nrch_gen_br_rsp_srv_base_encode,
		br_rsp_vex_ops, &wire_gen_rsp.payload,
		(void *)&wire_gen_rsp + sizeof(wire_gen_rsp), cmd_ctx)) < 0)
	{
		goto send_rsp;
	}
	wire_gen_rsp_len += rv;
	rv = 0;
send_rsp:
	__complete_send_nordda_cmd(cmd, NULL, &wire_gen_rsp, NVMEIB_REPLY_GEN,
						   wire_gen_rsp_len, rv, rv == 0 ? NVMEIBS_RSP_GEN_OPCODE_OK: NVMEIBS_RSP_GEN_OPCODE_ERR);
	kfree(cmd_ctx);
}

static void gen_cmd_process_work(struct workqe_struct *work);

static void gen_cmd_process(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct volume_client_req *req;
	struct nvmeibs_nr_cmd *cmd = NULL;
	unsigned idx;
	int rv = 0;
	u64 req_tag;
	void *end_copy_req;

	__NFIN;
	req = recv_ioctx->buf;
	req_tag = be64_to_cpu(req->hdr.tag);
	idx = nordda_tag_decode_index(req_tag);

	if (idx >= cl->nrch_ioreq_num) {
		_NE(error_nordda_gen_cmd_process, "Invalid request index @IDX_LLONG (max supported index is @NRCH_IOREQ_NUM)",
		    idx, cl->nrch_ioreq_num);
		rv = NVMEIBS_IO_RSP_ERR_INV_TAG;
		goto out;
	}

	cmd = &nrch->io_cmds[idx];
	cmd->gen_cmd = true;
	cmd->gen_op = req->gen_req.op;
	cmd->req_hdr_tag = req->hdr.tag;
	cmd->req_version_tag = req->version_tag;
	cmd->version = nordda_tag_decode_version(req_tag);
	cmd->ch_version = nordda_tag_decode_ch_version(req_tag);
	cmd_stats_md_on_cmd_process(cmd);

	_ND(trace_5_nordda_gen_cmd_process, "--- Deferring handling GEN_CMD @GEN_OP_STR (@OP) message,"
	" nrch @NRCH_NAME (@CH_PTR), req_index @REQ_INDEX, req_version @VERSION",
	    nvmeib_gen_op_str(cmd->gen_op), cmd->gen_op, nrch->name, nrch, idx, cmd->version);

	/* Copy request to command buffer so we can return the iu */
	end_copy_req = (void *)min_t(unsigned long,
			     (unsigned long)recv_ioctx->buf + recv_ioctx->recv_size,
			     (unsigned long)NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(req) + sizeof(struct volume_client_gen_req));
	cmd->cmd_req_sz = end_copy_req - NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(req);
	memcpy(cmd->cmd_req, NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(req), cmd->cmd_req_sz);

	/* Put cmd on WQ. (All gen cmds are defered now) */
	WQ_INIT_WORK(&cmd->work, gen_cmd_process_work);
	if (nvmeibs_nordda_add_work(nrch, &cmd->work) < 0) {
		_NE(error_2_nordda_gen_cmd_process, "Fail to add work");
		rv = NVMEIBS_IO_RSP_ERR_SUBMIT;
	}

out:
	/* return iu */
	post_recv_iu(nrch, recv_ioctx);

	if (rv < 0) {
		/* Send error response */
		struct volume_server_gen_rsp err_gen_rsp;
		size_t err_gen_rsp_len = offsetof(typeof(err_gen_rsp), payload);
		struct nvmeib_iu *send_ioctx = nordda_send_ioctx_get_or_disconnect(nrch->net);

		cmd_ended(nrch, req->hdr.tag);

		/* send response - we can't call __complete_send_nordda_cmd, because
		 * if the index is invalid, then we may not have a cmd */
		if (send_ioctx) {
			err_gen_rsp.comp_code = cpu_to_be32(rv);
			nrch_send_rsp(NR2C(nrch), nrch, NVMEIBS_RSP_GEN_OPCODE_ERR,
				      req->hdr.tag, req->version_tag, send_ioctx, &err_gen_rsp,
					err_gen_rsp_len, NVMEIB_REPLY_GEN, cmd->version);
		}
	}
	return;
}

static void __gen_cmd_process(struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	struct gens_cmd_ctx *cmd_ctx = NULL;
	struct nvmeib_iu *send_ioctx = NULL;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct volume_server_gen_rsp wire_gen_rsp;
	size_t wire_gen_rsp_len = offsetof(typeof(wire_gen_rsp), payload);
	enum nvmeib_wr_opcode send_wr_opcode = NVMEIB_REPLY_GEN;
	int rsp_ok_opcode = NVMEIBS_RSP_GEN_OPCODE_OK;
	int rv;
	size_t recv_sz = NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE;
	struct nvmeib_io_stats *stats = nrch->rionic->lionic->port->nis_dev->stats;

	_ND(trace_nordda_gen_cmd_process, "--- Start handling GEN_CMD @GEN_OP_STR (@OP) message, nrch @NRCH_NAME",
		nvmeib_gen_op_str(cmd->gen_op), cmd->gen_op, nrch->name);

	/* Here we are not on interrupt context so we can allocate cmd context */
	if (!(cmd_ctx = kzalloc(sizeof(*cmd_ctx), GFP_KERNEL))) {
		_NE(error_0_nordda_gen_cmd_process, "Out of memory while allocating cmd_ctx");
		rv = -ENOMEM;
		goto send_rsp;
	}
	cmd_ctx->cmd = cmd;

	_ND(trace_4_nordda_gen_cmd_process, "--- Handling GEN_CMD @GEN_OP_STR (@OP) message,"
					" nrch @NRCH_NAME (@CH_PTR), req_index @REQ_INDEX, req_version @VERSION",
					nvmeib_gen_op_str(cmd->gen_op), cmd->gen_op, nrch->name, nrch, cmd_ctx->cmd->idx, cmd_ctx->cmd->version);

	switch (cmd->gen_op) {
	case NVMEIB_GEN_OP_BLKSET_RECOVERED:
		if (be16_to_cpu(cmd->req_version_tag) != nrch->gen_br_ops->vex_ext) {
			_NT(gen_cmd_process_t1, "Invalid version tag @UINT in NVMEIB_GEN_OP_BLKSET_RECOVERED",
			  be16_to_cpu(cmd->req_version_tag));
			rv = -EPROTO;
			goto send_rsp;
		} else {
			const struct vex_ops *br_rsp_vex_ops = vex_select_ops(cl->link_version, vex_nrch_gen_br_rsp_srv_ops);

			if ((rv = CALL_VEX_OP(decode,
					vex_nrch_gen_br_req_srv_ops, BASE_ONLY,
						base, vex_nrch_gen_br_req_srv_base_decode,
							nrch->gen_br_ops, cmd->cmd_req,
							cmd->cmd_req + cmd->cmd_req_sz, cmd_ctx)) < 0)
				goto send_rsp;
			if ((rv = nvmeibs_handle_blkset_recovered_gen_cmd(
				     cl->di, &cmd_ctx->gen_req, &cmd_ctx->gen_rsp,
				     &NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(
				         cl->cid, false, nrch->cookie_ch,
				         .ctx.nordda.cmd_ctx = cmd_ctx))) < 0)
				goto send_rsp;
			if ((rv = CALL_VEX_OP(encode,
					vex_nrch_gen_br_rsp_srv_ops, BASE_ONLY,
						base, vex_nrch_gen_br_rsp_srv_base_encode,
							br_rsp_vex_ops, &wire_gen_rsp.payload,
							(void *)&wire_gen_rsp + sizeof(wire_gen_rsp), cmd_ctx)) < 0)
				goto send_rsp;
			wire_gen_rsp_len += rv;
		}
		break;

	case NVMEIB_GEN_OP_GET_UUID_JOUR:
		if (be16_to_cpu(cmd->req_version_tag) != nrch->gen_uj_ops->vex_ext) {
			_NT(gen_cmd_process_t2, "Invalid version tag @UINT in NVMEIB_GEN_OP_GET_UUID_JOUR",
			  be16_to_cpu(cmd->req_version_tag));
			rv = -EPROTO;
			goto send_rsp;
		} else {
			const struct vex_ops *uj_rsp_vex_ops = vex_select_ops(cl->link_version, vex_nrch_gen_uj_rsp_srv_ops);

			if ((rv = CALL_VEX_OP(decode,
					vex_nrch_gen_uj_req_srv_ops, ONE_EXT,
						base, vex_nrch_gen_uj_req_srv_base_decode,
						ext1, vex_nrch_gen_uj_req_srv_ext1_decode,
							nrch->gen_uj_ops, cmd->cmd_req,
							cmd->cmd_req + cmd->cmd_req_sz, cmd_ctx)) < 0)
				goto send_rsp;

			/* TBD: Add this to ext1 */
			cmd_ctx->gen_req.uj.binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;

			if ((rv = handle_get_uuid_jour(nrch, cmd_ctx->cmd, &cmd_ctx->gen_req, &cmd_ctx->gen_rsp)) < 0)
				goto send_rsp;
			
			CMD_STATS_MD_HEAD(cmd).send_rsp_len += cmd_ctx->gen_rsp.uj.jmdc_len + cmd_ctx->gen_rsp.uj.ent_md_len;

			/* Change the wr_id and send_ioctx to indicate we have transmitted from the BB */
			send_wr_opcode = NVMEIB_REPLY_GEN_W_RDMA;
			send_ioctx = cmd_ctx->cmd->io_iu;

			if ((rv = CALL_VEX_OP(encode,
				vex_nrch_gen_uj_rsp_srv_ops, TWO_EXT,
					base, vex_nrch_gen_uj_rsp_srv_base_encode,
					ext1, vex_nrch_gen_uj_rsp_srv_ext1_encode,
					ext2, vex_nrch_gen_uj_rsp_srv_ext2_encode,
					 uj_rsp_vex_ops, &wire_gen_rsp.payload,
					 (void *)&wire_gen_rsp + sizeof(wire_gen_rsp), cmd_ctx)) < 0) {
				goto send_rsp;
			}
			wire_gen_rsp_len += rv;
		}
		break;

	case NVMEIB_GEN_OP_GET_EC_DB:
		if (be16_to_cpu(cmd->req_version_tag) != nrch->gen_db_ops->vex_ext) {
			_NT(gen_cmd_process_t3, "Invalid version tag @UINT in NVMEIB_GEN_OP_GET_EC_DB",
			  be16_to_cpu(cmd->req_version_tag));
			rv = -EPROTO;
			goto send_rsp;
		} else {
			const struct vex_ops *db_rsp_vex_ops = vex_select_ops(cl->link_version, vex_nrch_gen_db_rsp_srv_ops);
			size_t rdma_len;
			if ((rv = CALL_VEX_OP(decode,
					vex_nrch_gen_db_req_srv_ops, BASE_ONLY,
						base, vex_nrch_gen_db_req_srv_base_decode,
							nrch->gen_db_ops, cmd->cmd_req,
							cmd->cmd_req + cmd->cmd_req_sz, cmd_ctx)) < 0)
				goto send_rsp;
			if ((rv = handle_get_ec_db(nrch, cmd_ctx->cmd, &cmd_ctx->gen_req, &cmd_ctx->gen_rsp, &rdma_len)) < 0)
				goto send_rsp;

			CMD_STATS_MD_HEAD(cmd).send_rsp_len += rdma_len;

			/* Change the wr_id and send_ioctx to indicate we have transmitted from the BB */
			send_wr_opcode = NVMEIB_REPLY_GEN_W_RDMA;
			send_ioctx = cmd_ctx->cmd->io_iu;

			if ((rv = CALL_VEX_OP(encode,
				vex_nrch_gen_db_rsp_srv_ops, BASE_ONLY,
					base, vex_nrch_gen_db_rsp_srv_base_encode,
					 db_rsp_vex_ops, &wire_gen_rsp.payload,
					 (void *)&wire_gen_rsp + sizeof(wire_gen_rsp), cmd_ctx)) < 0)
				goto send_rsp;
			wire_gen_rsp_len += rv;
		}
		break;
	case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
		if (be16_to_cpu(cmd->req_version_tag) != nrch->gen_fje_ops->vex_ext) {
			_NT(gen_cmd_process_t4, "Invalid version tag @UINT in NVMEIB_GEN_OP_FREE_JRNL_ENTS",
			  be16_to_cpu(cmd->req_version_tag));
			rv = -EPROTO;
			goto send_rsp;
		} else {
			int i;
			void *dec_ptr = NULL;
			void *dec_end_ptr = NULL;
			void *enc_data = NULL;
			struct nvmeib_free_ents_data *dec_free_ents = NULL;
			size_t dec_free_ents_sz, enc_free_ent_data_sz, enc_free_ents_sz;
			struct nvmeib_buffer bb_buff;

			if ((rv = CALL_VEX_OP(decode,
					vex_nrch_gen_fje_srv_ops, BASE_ONLY,
						base, vex_nrch_gen_fje_srv_base_decode,
							nrch->gen_fje_ops, cmd->cmd_req,
							cmd->cmd_req + cmd->cmd_req_sz, cmd_ctx)) < 0)
				goto send_rsp;
			/* Allocate a linear buffer to copy the BB data into */
			enc_free_ent_data_sz = vex_size(nrch->gen_fje_ent_ops, NULL, true);
			enc_free_ents_sz = enc_free_ent_data_sz * cmd_ctx->gen_req.free_ents.num_ents;
			/* Add to receive size counter for iostats */
			recv_sz += enc_free_ents_sz;
			enc_data = (void *)__get_free_pages(GFP_KERNEL, get_order(enc_free_ents_sz));
			if (!enc_data) {
				_NW(warn_2_gen_cmd_process_oom, "OOM");
				rv = -ENOMEM;
				goto free_fje_send_rsp;
			}
			/* Allocate a buffer to decode the BB data into */
			dec_free_ents_sz = sizeof(*cmd_ctx->gen_req.free_ents.ents) * cmd_ctx->gen_req.free_ents.num_ents;
			dec_free_ents = (void *)__get_free_pages(GFP_KERNEL, get_order(dec_free_ents_sz));
			if (!dec_free_ents) {
				_NW(warn_gen_cmd_process_oom, "OOM");
				rv = -ENOMEM;
				goto free_fje_send_rsp;
			}
			nvmeib_mem_sync_map_for_cpu(cmd->bb_map, 0, cmd->io_cmd_mem.size);
			nvmeib_buffer_init_sgl(&bb_buff, cmd->bb_map->mem_table.sgl,
						cmd->bb_map->mem_table.nents,
						cmd->bb_map->n_pages << PAGE_SHIFT, 0);
			dec_ptr = enc_data;
			dec_end_ptr = enc_data + enc_free_ents_sz;
			nvmeib_buffer_copy_to_buffer(&bb_buff, enc_data, enc_free_ents_sz);
			for (i = 0; i < cmd_ctx->gen_req.free_ents.num_ents; i++) {
				BUG_ON((void *)&dec_free_ents[i] > (void *)dec_free_ents + dec_free_ents_sz);
				if ((rv = CALL_VEX_OP(decode,
						vex_nrch_gen_fje_ent_srv_ops, ONE_EXT,
							base, vex_nrch_gen_fje_ent_srv_base_decode,
							ext1, vex_nrch_gen_fje_ent_srv_ext1_decode,
								nrch->gen_fje_ent_ops, dec_ptr, dec_end_ptr, &dec_free_ents[i])) < 0) {
					goto free_fje_send_rsp;
				}
				dec_ptr += rv;
			}
			cmd_ctx->gen_req.free_ents.ents = dec_free_ents;
			if ((rv = nvmeibs_handle_free_ents_gen_cmd(
				     cl->di, &cmd_ctx->gen_req,
				     &NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(
				         cl->cid, false, nrch->cookie_ch,
				         .ctx.nordda.cmd_ctx = cmd_ctx))) < 0) {
				goto free_fje_send_rsp;
			}
			rv = 0;
free_fje_send_rsp:
			if (enc_data)
				free_pages((unsigned long)enc_data, get_order(enc_free_ents_sz));
			if (dec_free_ents)
				free_pages((unsigned long)dec_free_ents, get_order(dec_free_ents_sz));
			goto send_rsp;
		}
		break;

	case NVMEIB_GEN_OP_JENTRY_ERASE:
		if (be16_to_cpu(cmd->req_version_tag) != nrch->gen_je_ops->vex_ext) {
			_NT(gen_cmd_process_t5, "Invalid version tag @UINT in NVMEIB_GEN_OP_JENTRY_ERASE",
			  be16_to_cpu(cmd->req_version_tag));
			rv = -EPROTO;
			goto send_rsp;
		} else {
			if ((rv = CALL_VEX_OP(decode,
					vex_nrch_gen_je_srv_ops, ONE_EXT,
						base, vex_nrch_gen_je_srv_base_decode,
						ext1, vex_nrch_gen_je_srv_ext1_decode,
							nrch->gen_je_ops, cmd->cmd_req,
							cmd->cmd_req + cmd->cmd_req_sz, cmd_ctx)) < 0)
				goto send_rsp;
			if ((rv = nvmeibs_serjio_erase_jam_ent(nrch->jrange_handle, cmd_ctx->gen_req.je.rng_binje,
				cmd_ctx->gen_req.je.rng_gen_id, 1, &cmd_ctx->gen_req.je.ent_erase,
				JAM_ENT_ERASE_REASON_FAILED_WRITE)) < 0)
				goto send_rsp;
		}
		break;

	default:
		_NE(error_1_nordda_gen_cmd_process, "Unexp opcode @OP", cmd->gen_op);
		rv = NVMEIBS_IO_RSP_ERR_INV_OP;
		goto send_rsp;
	}
	rv = 0;

send_rsp:
	/* Record iostats for GEN_RX - TBD: Latency measurement using delta between send/recv clocks */
	nvmeib_io_stats_adjust_and_update(stats, NULL, IO_STAT_VERB_GEN_RX, recv_sz, 0, false);

	if (rv != NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY) {
		__complete_send_nordda_cmd(cmd, send_ioctx, &wire_gen_rsp, send_wr_opcode, wire_gen_rsp_len,
								   rv, rv == 0 ? rsp_ok_opcode : NVMEIBS_RSP_GEN_OPCODE_ERR);
		kfree(cmd_ctx);
	}
	_ND(trace_3_nordda_gen_cmd_process, "--- Finish handling GEN_CMD @GEN_OP_STR (@OP) message, nrch @NRCH_NAME",
		nvmeib_gen_op_str(cmd->gen_op), cmd->gen_op, nrch->name);
	__NFOUT;
}

static void gen_cmd_process_work(struct workqe_struct *work)
{
	struct nvmeibs_nr_cmd *cmd = container_of(work, struct nvmeibs_nr_cmd, work);
	__gen_cmd_process(cmd);
}

static void io_cmd_process(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	NFIN;
	switch (req->hdr.opcode) {
	case NVMEIB_CMD:
		if (unlikely(req->client_op == NVMEIBC_CR_LOCK)) {
			execute_lock(nrch, recv_ioctx);
		} else {
			_io_cmd_process(nrch, recv_ioctx);
		}
		break;
	case NVMEIB_GEN_CMD:
		gen_cmd_process(nrch, recv_ioctx);
		break;
	default:
		_NE(error_3_nordda_io_cmd_process, "@NRCH_NAME invalid op=@OPCODE", nrch->name, req->hdr.opcode);
	}

	NFOUT;
}

static void attempt_handle_pending_recv(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeib_iu *recv_ioctx;
	unsigned long flags;
	__NFIN;

	spin_lock_irqsave(&nrch->spinlock, flags);
	while (unlikely(
			!list_empty(&nrch->net->pending_received_msgs) &&
			nrch->net->state == QP_LIVE)) {
		if ((recv_ioctx = list_first_entry_or_null(
			&nrch->net->pending_received_msgs, struct nvmeib_iu, free_tx_n))) {
			list_del_init(&recv_ioctx->free_tx_n);
			spin_unlock_irqrestore(&nrch->spinlock, flags);
			handle_io_cmd_underway(nrch, recv_ioctx);
			spin_lock_irqsave(&nrch->spinlock, flags);
		}
		else BUG();
	}
	spin_unlock_irqrestore(&nrch->spinlock, flags);

	__NFOUT;
}

static void io_cmd_process_work(struct workqe_struct *work)
{
	struct nvmeib_iu *recv_ioctx = container_of(work, struct nvmeib_iu, work);
	struct nvmeibs_net *net = recv_ioctx->owner_ptr;
	struct nvmeibs_nr_channel *nrch = net->params.nrch;
	struct volume_client_req *req = recv_ioctx->buf;
	NFIN;

	if (nvmeibs_net_get_qp_state(nrch->net) == QP_LIVE) {
		io_cmd_process(nrch, recv_ioctx);
	} else {
		_NT(trace_nordda_io_cmd_process_work, "nrch @NRCH_NAME, not processing deferred cmd, net not LIVE", nrch->name);
		cmd_ended(nrch, req->hdr.tag);
		post_recv_iu(nrch, recv_ioctx);
	}

	NFOUT;
}

static inline void io_cmd_defer(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	NFIN;

	WQ_INIT_WORK(&recv_ioctx->work, io_cmd_process_work);
	if (nvmeibs_nordda_add_work(nrch, &recv_ioctx->work) < 0) {
		_NE(error_1_nordda_io_cmd_defer, "Fail to add work");
		goto err;
	}
	goto out;

err:

	post_recv_iu(nrch, recv_ioctx);

out:
	NFOUT;
}

static inline void io_cmd_pending(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	unsigned long flags;

	spin_lock_irqsave(&nrch->spinlock, flags);
	list_add_tail(&recv_ioctx->free_tx_n, &nrch->net->pending_received_msgs);
	spin_unlock_irqrestore(&nrch->spinlock, flags);
}

static enum can_handle_res can_handle_io_cmd(
	struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	enum can_handle_res rv;
	bool defer;
	NFIN;

	defer = recv_ioctx->defer;
	recv_ioctx->defer = false;

	if (unlikely(nvmeibs_net_get_qp_state(nrch->net) == QP_CONNECTING)) {
		rv = NORDDA_PENDING;
	}
	else {
		rv = !defer ? NORDDA_NOW : NORDDA_DEFER;
	}

	NFOUT;
	return rv;
}

//TBD: Fix reorder of recv-msg due - see cl->fwd2_pending_received_msgs

// Handle new io command.
// This function can be called multiple times for a given command (e.g. after
// being retried from the pending_received_msgs list).
static void handle_io_cmd_underway(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	enum can_handle_res res;
	struct volume_client_req *req = recv_ioctx->buf;
	__NFIN;

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sink for Remote RDMA_SEND */
	ib_dma_sync_single_for_cpu(P2IB(NR2P(nrch)), recv_ioctx->dma,
		recv_ioctx->recv_size, DMA_FROM_DEVICE);

	switch ((res = can_handle_io_cmd(nrch, recv_ioctx))) {
	case NORDDA_NOW:
		io_cmd_process(nrch, recv_ioctx);
		break;

	case NORDDA_DEFER:
		io_cmd_defer(nrch, recv_ioctx);
		break;

	case NORDDA_PENDING:
		io_cmd_pending(nrch, recv_ioctx);
		break;

	default:
		_NE(error_nordda_handle_io_cmd_underway, "Unexpected res @RES", res);
		cmd_ended(nrch, req->hdr.tag);
		post_recv_iu(nrch, recv_ioctx);
		break;
	}

	__NFOUT;
}

static void handle_io_cmd(struct nvmeibs_nr_channel *nrch,
	struct nvmeib_iu *recv_ioctx)
{
	__NFIN;
	// handle new command only if channel hasn't been shut down already
	if (new_cmd_underway(nrch, recv_ioctx)) {
		handle_io_cmd_underway(nrch, recv_ioctx);
	} else {
		/* put recv-ctx back to receive queue */
		post_recv_iu(nrch, recv_ioctx);
	}
	__NFOUT;
}

static int process_recv_completion(struct nvmeibs_nr_channel *nrch,
	struct ib_wc *wc, bool defer)
{
	struct nvmeib_iu *recv_ioctx;
	unsigned index;
	struct nvmeib_hdr *hdr;
	int rv = 0;
	__NFIN;

	index = nvmeib_idx_from_wc(wc);
	if (!(recv_ioctx = get_recv_iu(nrch, index))) {
		_NE(error_nordda_process_recv_completion, "Got recv completion without iu (opcode @OPCODE, status @STATUS)",
		   wc->opcode, wc->status);
		rv = -ENOENT;
		goto out;
	}
	recv_ioctx->recv_size = wc->byte_len;

	if (atomic_read(&nrch->net->dying)) {
		/* Net is dying - put recv-ctx back to receive queue (for SRQ) */
		post_recv_iu(nrch, recv_ioctx);
		rv = -ENETDOWN;
		goto out;
	}

	if (wc->status != IB_WC_SUCCESS) {
		_NT(trace_nordda_process_recv_completion, "recv completion (idx @INDEX) w/ err @STATUS", index, wc->status);
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_RCV_COMPLETION_FAILED);
		/* put recv-ctx back to receive queue */
		post_recv_iu(nrch, recv_ioctx);
		rv = -ECONNRESET;
		goto out;
	}

	hdr = recv_ioctx->buf;
	if (hdr->opcode == NVMEIB_CMD ||
		hdr->opcode == NVMEIB_GEN_CMD) {
		nvmeibs_nr_lat_meas_fill_rx_md(wc, recv_ioctx);

		recv_ioctx->defer = defer;
		handle_io_cmd(nrch, recv_ioctx);
		rv = 0;
	}
	else {
		_NE(error_1_nordda_process_recv_completion,
				"Unexpected opcode @OPCODE, status @STATUS, index @INDEX,"
				" recv_ioctx @RECV_IOCTX, nrch @NRCH rq_posted_count @ATOMIC_READ",
		   hdr->opcode, wc->status, index, recv_ioctx, nrch, atomic_read(&nrch->net->rq_post_count));
		BUG_ON(1);
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_RCV_COMPLETION_FAILED);
		/* put recv-ctx back to receive queue */
		post_recv_iu(nrch, recv_ioctx);
		rv = -EINVAL;
	}

out:
	__NFOUT;
	return rv;
}

static void __nordda_recv_completion(struct ib_cq *cq,
	struct nvmeibs_nr_channel *nrch)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct ib_wc *wcs = nrch->rcq_wcs;
	struct nvmeibs_net *net = nrch->net;
	int i, n, rv;
	bool defer;
	cycles_t start_tsc = nvmeib_public_get_cycles();
	__NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	while ((n = ib_poll_cq(cq, NVMEIBS_POLL_SIZE, wcs)) > 0) {
		nvmeib_qp_stats_on_poll_cq(net->qp_stats, n, recv);
		nvmeibs_net_dec_recv(net, n);
		for (i = 0; i < n; ++i) {
			if (nvmeib_opcode_from_wc(&wcs[i]) == NVMEIB_DRAIN_QUEUE)
				nvmeibs_rq_drain_comp(net, &wcs[i]);
			else if (!cl->dismissed) {
				defer = !nvmeibs_defer_recv_comps ? false :
					nvmeib_intr_shaper_calc_percpu(s_intr_shaper, n, nvmeib_public_get_cycles() - start_tsc);
				if ((rv = process_recv_completion(nrch, &wcs[i], defer))) {
					_ND(trace_nordda_nordda_recv_completion, "process_recv_completion failed (@RV)", rv);
				}
			}
			else {
				u32 index = nvmeib_idx_from_wc(&wcs[i]);
				struct nvmeib_iu *recv_ioctx = get_recv_iu(nrch, index);
				if (recv_ioctx)
					post_recv_iu(nrch, recv_ioctx);
				else
					_NE(error_nordda_nordda_recv_completion, "Got null recv_ioctx");
			}
		}
		if ((rv = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS)) <= 0) {
			if (rv < 0)
				_NE(error_1_nordda_nordda_recv_completion, "ib_req_notify_cq failed (@RV) for nrch @NRCH", rv, nrch);
			break;
		}
	}
	if (n < 0) {
		_NT(trace_1_nordda_nordda_recv_completion, "cl @CL_NAME, poll-recv-cq err (@ERR)", cl->name, n);
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_RCV_COMPLETION_FAILED);
	} else {
		nvmeib_qp_stats_on_poll_cq_empty(net->qp_stats);
	}
	__NFOUT;
}

static void __nordda_send_completion(struct ib_cq *cq,
	struct nvmeibs_nr_channel *nrch, bool from_recv);
static void nordda_recv_completion(struct ib_cq *cq, void *ctx)
{
	struct nvmeibs_nr_channel *nrch = ctx;
	__NFIN;

	nvmeib_qp_stats_on_interrupt(nrch->net->qp_stats);
	__nordda_recv_completion(cq, nrch);
	if (nrch->net && nrch->net->scq)
		__nordda_send_completion(nrch->net->scq, nrch, true);
	else {
		_NT(trace_2_nordda_nordda_recv_completion, "Oops: nrch @NRCH, no scq", nrch);
		WARN_ON(true);
	}

	__NFOUT;
}

static void nordda_record_io_stats(struct nvmeibs_nr_cmd *cmd, u16 version)
{
	struct nvmeibs_nr_channel *nrch = cmd->nrch;
	struct nvmeib_io_stats *stats = nrch->rionic->lionic->port->nis_dev->stats;

	struct nvmeibs_nr_cmd_stats_md *stats_md;
	unsigned i;

	__NFIN;
	/* Look for stats in history. 
	 * NOTE: We could keep a consumer and only look between producer and consumer, 
	 * but it's only 4 entries so not worth it */
	for (i = 0, stats_md = cmd->stats_md_ring; i < CMD_STATS_HIST_LEN; i++, stats_md++) {
		if (stats_md->valid && stats_md->version == version) {
			/* Found it */
			if (stats_md->stat_verb < N_IO_STAT_VERBS) {
				ktime_t start_time = stats_md->send_rsp_time;
				ktime_t end_time = nvmeib_public_ktime_get();
				u64 lat = ktime_after(end_time, start_time) ? ktime_to_ns(ktime_sub(end_time, start_time)) : 0;
				u64 size = stats_md->stat_verb == IO_STAT_VERB_GEN_TX ? stats_md->send_rsp_len : stats_md->io_xfer_size;

				nvmeib_io_stats_adjust_and_update(stats, NULL, stats_md->stat_verb, size, lat, false);
			}
			/* Ensure to only count it once */
			stats_md->valid = false;
			break;
		}
	}
	__NFOUT;
}

static void nordda_record_gen_no_rdma_stats(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *send_ioctx)
{
	struct nvmeib_io_stats *stats = nrch->rionic->lionic->port->nis_dev->stats;
	ktime_t start_time = send_ioctx->send_time;
	ktime_t end_time = nvmeib_public_ktime_get();
	u64 lat = ktime_after(end_time, start_time) ? ktime_to_ns(ktime_sub(end_time, start_time)) : 0;

	nvmeib_io_stats_adjust_and_update(stats, NULL, IO_STAT_VERB_GEN_TX, send_ioctx->send_size, lat, false);
}

/* -------------------------------------------------------------------------- */
/* Send                                                                       */
/* -------------------------------------------------------------------------- */
static void process_send_completion(struct nvmeibs_nr_channel *nrch,
	struct ib_wc *wc, bool *do_pending_recv)
{
	struct nvmeib_iu *send_ioctx;
	u32 index;
	u32 opcode;
	bool err = false;
	bool pop = false;
	//__NFIN;

	index = nvmeib_idx_from_wc(wc);
	opcode = nvmeib_opcode_from_wc(wc);

	_ND(trace_nordda_process_send_completion, "nordda send-comp @NVMEIB_WR_OPCODE_STR, idx @INDEX, opcode @OPCODE, status @STATUS",
		nvmeib_wr_opcode_str(opcode), index, opcode, wc->status);

	if (wc->status != IB_WC_SUCCESS)
		err = opcode != NVMEIB_DRAIN_QUEUE;
	if (opcode == NVMEIB_REPLY_IO || opcode == NVMEIB_REPLY_GEN_W_RDMA ||
			opcode == NVMEIB_REPLY_IO_W_RDMA)
	{
		struct nvmeibs_nr_cmd *cmd = &nrch->io_cmds[index];
		u16 version = nordda_wr_id_decode_version(nvmeib_wr_id_from_wc(wc));
		/* completion of io-cmd rsp (sent on disk-completion).
		   iu of the io-cmd's, do not put back to net's pool */
		pop = true;
		if (wc->status == IB_WC_SUCCESS) {
			nordda_record_io_stats(cmd, version);
			nvmeibs_nr_lat_meas_record_send_comp(wc, cmd);
		}
	}
	else if (opcode == NVMEIB_REPLY_NO_IO || /* comp of _io-cmd-process error-rsp */
			opcode == NVMEIB_REPLY_GEN) { 	/* comp of gen-cmd rsp */
		pop = true;
		if (wc->status == IB_WC_SUCCESS) {
			/* In this case, the index is not the cmd, but rather the index of the ioctx_ring */
			send_ioctx = nrch->net->ioctx_ring[index];
			if (opcode == NVMEIB_REPLY_GEN) {
				nordda_record_gen_no_rdma_stats(nrch, send_ioctx);
			}
			nvmeibs_net_put_ioctx(nrch->net, send_ioctx);
			if (do_pending_recv)
				*do_pending_recv = true;
		}
	}
	else if (opcode == NVMEIB_DRAIN_QUEUE) {
		BUG_ON(wc->status != IB_WC_WR_FLUSH_ERR); /* Drain should only happen when QP is in error state */
		if (do_pending_recv)
			*do_pending_recv = false;
		nvmeibs_net_on_drain_sq(nrch->net);
	}
	else {
		_NT(trace_1_nordda_process_send_completion, "Unexpected opcode @NVMEIB_WR_OPCODE_STR", nvmeib_wr_opcode_str(opcode));
                /* completions of NVMEIB_RDMA_MID, NVMEIB_RDMA_LAST
                   and NVMEIB_RDMA_METADATA are not signaled */
		err = true;
	}

	if (unlikely(err)) {
		_NT(trace_2_nordda_process_send_completion, "nrch @NRCH_NAME (@NRCH), send-comp @NVMEIB_WR_OPCODE_STR, idx @INDEX, status @STATUS", nrch->name,
			nrch, nvmeib_wr_opcode_str(opcode), index, wc->status);
		if (wc->status == IB_WC_REM_ACCESS_ERR &&
			(opcode == NVMEIB_RDMA_LAST || (opcode >= NVMEIB_RDMA_MID_0 && opcode <= NVMEIB_RDMA_MID_MAX))) {
			const struct nvmeibs_nr_cmd *nr_cmd = &nrch->io_cmds[index];
			struct nvmeibs_client *cl = NR2C(nrch);
			int n_md_iu = cl->di->metadata ? (cl->di->mtdt_extd ? nr_cmd->io_iu->n_rdma_iu : 1) : 0;
			int n_iu = nr_cmd->io_iu->n_rdma_iu + n_md_iu;
			int iu_idx = opcode == NVMEIB_RDMA_LAST ? n_iu - 1 : opcode - NVMEIB_RDMA_MID_0;
			const struct nvmeib_rdma_iu *iu = &nr_cmd->io_iu->rius[iu_idx];
			int sg_idx;
			u32 rdma_len = 0;

			/* nr_cmd and nr_cmd->io_iu are still valid because the error means that the response was not received */
			for (sg_idx = 0; sg_idx < iu->sge_cnt; sg_idx++)
				rdma_len += iu->sge[sg_idx].length;

			if (!nr_cmd->gen_cmd) {
				_NW(warn_3_nordda_process_send_completion,
					"RDMA REMOTE ACCESS ERROR: nrch @NRCH_NAME (@NRCH), idx @INDEX, ind_op @IND_OP"
					" R-Key: @RKEY, R-Addr: @RADDR, L-Key: @LKEY, Length: @RDMA_LEN",
					nrch->name, nrch, index, nr_cmd->ind_op, iu->rkey, iu->raddr, iu->lkey, rdma_len);
			} else {
				_NW(warn_4_nordda_process_send_completion,
					"RDMA REMOTE ACCESS ERROR: nrch @NRCH_NAME (@NRCH), idx @INDEX, gen_op @GEN_OP_STR"
					" R-Key: @RKEY, R-Addr: @RADDR, L-Key: @LKEY, Length: @RDMA_LEN",
					nrch->name, nrch, index, nvmeib_gen_op_str(nr_cmd->gen_op), iu->rkey, iu->raddr, iu->lkey, rdma_len);
			}
		}
		if (do_pending_recv)
			*do_pending_recv = false;
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_SND_COMPLETION_FAILED);
		if ((wc->status == IB_WC_RETRY_EXC_ERR ||
				wc->status == IB_WC_RESP_TIMEOUT_ERR ||
				wc->status == IB_WC_FATAL_ERR)) {
			_NT(error_nordda_process_send_completion, "Detected Transport Error. Disconnecting all io-channels on IO-Path");
			nvmeibs_client_disconnect_io_path(nrch->net->params.cl, nrch->rionic);
		}
	}

	if (nvmeibs_use_pcpu_cq &&
		nvmeibs_nr_post_recv_on_send_comp &&
		pop) {
		struct nvmeib_iu *recv_ioctx = rxiu_pop(nrch);
		if (recv_ioctx)
			post_recv_iu(nrch, recv_ioctx);
		else {
			_NE(process_send_completion_e1, "nrch @STR (@PTR), Fail to pop rxiu - send-comp @STR, idx @UINT, "
			   "status @INT", nrch->name, nrch, nvmeib_wr_opcode_str(opcode),
			   index, wc->status);
		}
	}

	//__NFOUT;
}


/* client may reuse io-req before server managed to poll
 * the send-comp of the io-rsp it had sent for prev usage
 * of the same io-req.
 */
static void poll_send_cq_intr(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct ib_cq *cq = nrch->net->scq;
	DECLARE_IB_WC_ONSTACK(scq_wcs);
	int n, m = NVMEIBS_POLL_SIZE;

	__NFIN;

	if (nvmeibs_use_pcpu_cq) {
		_NE(error_nordda_poll_send_cq_intr, "OOPS, nrch @NRCH_NAME post-send caleed from '@__BUILTIN_RETURN_ADDRESS_FUNC' returned ENOMEM "
		   "over shared percpu cq - bail, dont poll-cq",
		   nrch->name, __builtin_return_address(0));
		WARN_ON_ONCE(1);
		return;
	}

	while (m--) {
		ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
		n = ib_poll_cq(cq, 1, &scq_wcs);
		if (n < 0) {
			_NT(trace_nordda_poll_send_cq_intr, "cl @CL_NAME, poll-send-cq err (@ERR)", cl->name, n);
			nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_SND_CQ_INTR_FAILED);
		}
		else if (n) {
			nvmeib_qp_stats_on_poll_cq(nrch->net->qp_stats, n, send);
			process_send_completion(nrch, &scq_wcs, NULL);
		}
		else {
			nvmeib_qp_stats_on_poll_cq_empty(nrch->net->qp_stats);
			break;
		}
	}

	/* TBD:
	   if driver have more pending CQEs after our last poll,
	   we will nit get interrupt for them and they will only
	   be polled due to newly posted SIGNALED WRs or calling
	   this func again. */
	__NFOUT;
}

static void __nordda_send_completion(
	struct ib_cq *cq, struct nvmeibs_nr_channel *nrch, bool from_recv)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct ib_wc *wcs = from_recv ? nrch->rcq_wcs : nrch->scq_wcs;
	unsigned long flags;
	int i, n;
	bool do_pending_recv = false;
	__NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	spin_lock_irqsave(&nrch->spinlock, flags);

again:
	n = ib_poll_cq(cq, NVMEIBS_POLL_SIZE, wcs);
	if (n < 0) {
		_NT(trace_nordda_nordda_send_completion, "cl @CL_NAME, poll-send-cq err (@ERR)", cl->name, n);
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_SND_COMPLETION_FAILED);
		goto unlock;
	} else if (n > 0) {
		nvmeib_qp_stats_on_poll_cq(nrch->net->qp_stats, n, send);
	} else {
		nvmeib_qp_stats_on_poll_cq_empty(nrch->net->qp_stats);
	}
	if (cl->dismissed)
		_NT(trace_1_nordda_nordda_send_completion, "Allow processing of drain-sq wr");
	for (i = 0; i < n; ++i) {
		process_send_completion(nrch, &wcs[i], &do_pending_recv);
	}
	if (n == NVMEIBS_POLL_SIZE)
		goto again;
	if (ib_req_notify_cq(cq, IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS) > 0)
		goto again;

unlock:
	spin_unlock_irqrestore(&nrch->spinlock, flags);

	if (do_pending_recv)
		attempt_handle_pending_recv(nrch);

	__NFOUT;
}

static void nordda_send_completion(struct ib_cq *cq, void *ctx)
{
	struct nvmeibs_nr_channel *nrch = ctx;
	__nordda_send_completion(cq, nrch, false);
}

/* -------------------------------------------------------------------------- */
/* Connect                                                                    */
/* -------------------------------------------------------------------------- */
static int nrch_rscs_alloc(struct nvmeibs_nr_channel *nrch, struct nvmeib_dev *dev, int qp_num)
{
	int rv = -1;
	proc_name_t pname;
	NFIN;

	if (!nrch || nrch->wq || nrch->priv_srq) {
		_NE(error_nordda_nrch_rscs_alloc, "invalid nrch @NRCH", nrch);
		goto out;
	}
	srv_proc_name_format(pname, 'S', "WQ", "nr", qp_num);
	nrch->wq = nvmeibs_nr_wq_set_cpu_affinity ? wq_create_on(pname, qp_num) : wq_create(pname);
	if (!nrch->wq) {
		_NE(error_1_nordda_nrch_rscs_alloc, "Fail to allocate wq, nrch @NRCH", nrch);
		goto out;
	}
	if (nvmeibs_max_nic_srqs > 1 && USE_PRIV_SRQ) {
		struct nvmeib_srq_params params = {
			.q_size = PRIV_SRQ_SIZE,
			.msg_size = NVMEIBC_NORDDA_CLIENT_MSG_SIZE
		};
		if (!(nrch->priv_srq = nvmeib_srq_info_create(dev, &params, nrch, s_nordda_srq_info)))
			_NT(trace_nordda_nrch_rscs_alloc, "Fail to create private srq, fallback to dev's SRQ-pool");
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

static void nrch_rscs_free(struct nvmeibs_nr_channel *nrch)
{
	NFIN;

	if (nrch) {
		if (nrch->wq) {
			wq_drain(nrch->wq);
			wq_destroy(nrch->wq);
			nrch->wq = NULL;
		}
		if (nrch->priv_srq) {
			BUG_ON(nrch->net->srq_info != nrch->priv_srq);
			/* NULLify net srq_info to stop nvmeib_srq_info_put being called by destroy_ib_rscs */
			nrch->net->srq_info = NULL;
			nvmeib_srq_info_free(nrch->priv_srq);
			nrch->priv_srq = NULL;
		}
		if (nrch->recv_q) {
			nvmeib_free_recvq(nrch->recv_q);
			kfree(nrch->recv_q);
			nrch->recv_q = NULL;
		}
		nvmeibs_nr_lat_meas_nrch_free(nrch);

		nvmeibs_async_cookie_store_remove_ch(nrch->cookie_ch);
		nrch->cookie_ch = NULL;
	}

	NFOUT;
}

static void nordda_ready_to_send(void *context)
{
	struct nvmeibs_nr_channel *nrch = context;
	__NFIN;
	attempt_handle_pending_recv(nrch);
	__NFOUT;
}

/* Calculate the required size of the Send Queue */
static int calc_sq_size(struct nvmeibs_client *cl, int *max_send_wrs,
			struct nvmeibs_ib_port *ib_port)
{
	int n_wrs_rd_io = 1; /* Each IO requires at least a RDMA Send for the response */
	struct nvmeibs_disk_info *di = cl->di;
	int max_pages_per_io = DIV_ROUND_UP(
		(di->max_request_size << di->block_shift), PAGE_SIZE);
	int sq_size;

	if (di->metadata && di->mtdt_extd) {
		/* If the metadata is inline (extends the page), then we need to un-mix it.
		* This requires 2 WRs per page of data
		* One to RDMA Write the data to the BIO and the other to RDMA Write the MD to the MD buffer */
		n_wrs_rd_io += max_pages_per_io * 2;
	} else if (NVMEIBS_NORDDA_ASSUME_DATA_MR(ib_port->hw_type) && !cl->max_wrs_per_req) {
		/* Otherwise (Seperate MD or no MD), we only need 1 WR for the data
		 * (if we assume the client always uses an MR for the data) */
		n_wrs_rd_io += 1 + (di->metadata ? 1 : 0);
	} else if (cl->max_wrs_per_req) {
		/* From user to allow mem usage control */
		n_wrs_rd_io += cl->max_wrs_per_req + (di->metadata ? 1 : 0);
	} else {
		/* We assume the client cannot always use an MR for the data so will need 1 WR per page of the BB */
		n_wrs_rd_io += max_pages_per_io + (di->metadata ? 1 : 0);
	}

	if (max_send_wrs)
		*max_send_wrs = n_wrs_rd_io;
	sq_size = n_wrs_rd_io * cl->nrch_ioreq_num;

	_NT(trace_nordda_calc_sq_size, "Disk @DISK_ID_STR: h=@HW_TYPE m=@MAX_REQUEST_SIZE, s=@BLOCK_SHIFT_INT, p=@MAX_PAGES_PER_IO c=@UINT --> n=@N_WRS_RD_IO, s=@SQ_SIZE",
		di->disk_id, ib_port->hw_type, di->max_request_size,
		di->block_shift, max_pages_per_io, cl->max_wrs_per_req,
		n_wrs_rd_io, sq_size);
	return sq_size;
}

static void nordda_bn_handler(void *context);
static void nordda_ad_handler(void *context);
static void nordda_after_destroy_ib_handler(void *context);

static void connect_nordda_channel_work(struct workqe_struct *work)
{
	struct alloc_work *w = container_of(work, struct alloc_work, work);
	struct nvmeibs_ib_port *ib_port = w->port;
	struct nvmeib_rdma_cm *cm_id = w->cm_id;
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibc_login_request *req = &w->req;
	struct nvmeibs_login_reject lrej = {{0}};
	struct nvmeibs_login_reject *rej = &lrej;
	struct nvmeibs_login_response *rsp = NULL;
	struct nvmeibs_net_init *params = NULL;
	struct nvmeibs_net_init_target target;
	struct nvmeibs_rionic *rionic;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_lionic *lionic;
	struct nvmeibs_nr_channel *nrch = NULL;
	struct nvmeibc_io_channel_def def;
	int qp_num, i, j;
	__NFIN;

	nvmeibs_client_prp_io_channel_def(cl, req, ib_port, &def);
	if (!(rionic = nvmeibs_client_get_rionic(cl, &def, NVMEIBS_IOCH_NORDDA))) {
		_NE(error_nordda_connect_nordda_channel_work, "rejected NVMEIB_IO_LOGIN_REQ because no match for src/dst nic.");
		rej->reason = __constant_cpu_to_be32(
			NVMEIBS_LOGIN_REJ_IO_CHANNEL_SRC_DST_MATCH);
		goto reject;
	}
	qp_num = be16_to_cpu(def.qp_num);
	nrch = &rionic->nr_channels[qp_num];
	if (nrch->id != -1) {
		_NE(error_1_nordda_connect_nordda_channel_work, "NR channel (@QP_NUM-@ID_INT) is still connected - rejecting new connection",
			qp_num, nrch->id);
		rej->reason = __constant_cpu_to_be32(
			NVMEIBS_LOGIN_REJ_IO_CHANNEL_ALREADY_CONNECTED);
		/* start net release if it is not already in progress */
		_NE(error_2_nordda_connect_nordda_channel_work, "net release @NET, nrch @NRCH", nrch->net, nrch);
		nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_NR_CH_ALREADY_CONNECTED);
		nrch = NULL; /* so we wont kfree it after jump */
		goto reject;
	}

	/* in the case we reuse the channel */
	if (nrch->net) {
		kfree(nrch->net);
		nrch->net = NULL;
	}

	/* allocate and initialize
	   nordda channel resources */
	if (nrch_rscs_alloc(nrch, P2NV(ib_port), qp_num) < 0) {
		_NE(error_3_nordda_connect_nordda_channel_work, "Fail to allocate nrch resources");
		goto reject;
	}
	spin_lock_init(&nrch->spinlock);
	atomic_set(&nrch->underway_cmds, 1);	// will be decreased to zero by wait_for_all_underway()
	INIT_LIST_HEAD(&nrch->rxiu_list);
	nrch->n_rxiu = 0;
	nrch->n_rxiu_tot = 0;
	nrch->rxiu_dying = 0;

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!rsp || !params) {
		rej->reason = NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES;
		_NE(error_4_nordda_connect_nordda_channel_work, "rejected NVMEIB_IO_LOGIN_REQ because no memory.");
		goto reject;
	}
	lionic = rionic->lionic;
	cdisk = lionic->disk;
	i = ((void *)lionic - (void *)cdisk->lionics) / sizeof(*lionic);
	j = ((void *)rionic - (void *)lionic->rionics) / sizeof(*rionic);

	/* init net params */
	snprintf(params->name, sizeof(params->name), "NR.%d:%d:%03d",
		i, j, qp_num);
	snprintf(nrch->name, sizeof(nrch->name), "%.*s~%.*s",
		NVMEIBS_CLIENT_NAME_SIZE, cl->name,
		NVMEIBS_CONNECTION_LOCAL_NAME_SIZE, params->name);
	params->net_type = S_NET_NORDDA;
	params->cl = cl;
	params->nrch = nrch;
	WARN_ON(ib_port == NULL);
	params->port = ib_port;
	params->cm_id = cm_id;
	/* send */
	params->s_msg_size = NVMEIBS_NORDDA_SERVER_MSG_SIZE;
	params->max_send_sge = NVMEIBS_SERVER_DEFAULT_MAX_SGES;
	params->sendq_size = calc_sq_size(cl, &nrch->max_send_wrs, ib_port);
	params->scq_size = params->sendq_size;
	/* recv */
	params->rcq_size = params->sendq_size;
	if (nvmeibs_support_srq(P2NV(ib_port))) {
		params->use_srq = true;
		params->srq_priv = nrch->priv_srq;
		params->srq_type = nrch->priv_srq ? NVMEIB_SRQ_TYPE_INVALID :
			NVMEIB_SRQ_TYPE_SECONDARY;
	} else {
		params->recvq_size = 2 * cl->nrch_ioreq_num;
		params->r_msg_size = ib_port->port_attrib.max_req_size;
		params->max_recv_sge = 1;
	}
	/* handlers */
	params->cm_handler = NULL;
	params->scq_context = nrch;
	params->rcq_context = nrch;
	if (!nvmeibs_use_pcpu_cq) {
		params->scq_handler = nordda_send_completion;
		params->rcq_handler = nordda_recv_completion;
	}
	else {
		params->send_comp_h = nordda_send_comp_h;
		params->recv_comp_h = nordda_recv_comp_h;
	}
	params->rts_handler = nordda_ready_to_send;
	params->rts_context = nrch;
	params->rw_handler = NULL;
	params->rw_context = NULL;
	params->bn_handler = nordda_bn_handler;
	params->bn_context = nrch;
	params->ad_handler = nordda_ad_handler;
	params->ad_context = nrch;
	params->ka_handler = NULL;
	params->ka_context = NULL;
	params->after_destroy_ib = nordda_after_destroy_ib_handler;
	params->after_destroy_ib_ctx = nrch;
	params->ch_index = qp_num;

	if (nvmeibs_nr_lat_meas_nrch_alloc(cl, nrch))
		goto reject;

	if (!params->use_srq) {
		if (!(nrch->recv_q = kzalloc(sizeof(*cl->recv_q), GFP_KERNEL))) {
			_NE(error_5_nordda_connect_nordda_channel_work, "Memory allocation error for receive queue");
			goto reject;
		}
		if ((nvmeib_init_recvq(nrch->recv_q, P2NV(ib_port),
			params->recvq_size, params->r_msg_size))) {
			_NE(error_6_nordda_connect_nordda_channel_work, "Error initializing receive queue");
			kfree(nrch->recv_q);
			goto reject;
		}
		params->recv_q = nrch->recv_q;
	}

	if (!(nrch->scq_wcs = kcalloc(NVMEIBS_POLL_SIZE, sizeof(*nrch->scq_wcs), GFP_KERNEL)) ||
		!(nrch->rcq_wcs = kcalloc(NVMEIBS_POLL_SIZE, sizeof(*nrch->rcq_wcs), GFP_KERNEL))) {
		_NE(err_nordda_connect_nordda_channel_work_wcs_oom, "OOM");
		goto reject;
	}

	nrch->n_scq_wcs = NVMEIBS_POLL_SIZE;
	nrch->n_rcq_wcs = NVMEIBS_POLL_SIZE;

	if (P2NV(ib_port)->dev_type == DT_siw &&
		nvmeibs_nr_lat_meas_alloc_siw_md(nrch) != 0)
		goto reject;

	if (!(nrch->cookie_ch = nvmeibs_async_cookie_store_add_ch(
	          &cl->cookie_store,
	          (nvmeibs_async_cookie_channel_t)nrch, GFP_KERNEL))) {
		_NE(error_7_nordda_connect_nordda_channel_work, "Memory allocation error for cookies channel");
		goto reject;
	}

	/* create nvmeib_login_response */
	nvmeib_wire_op_cid_set_rsp(&rsp->base.op_cid, NVMEIB_LOGIN_RSP, 0);
	rsp->base.opcode = NVMEIBS_NORDDA_CHANNEL;

	if (nvmeibs_client_rionic_fill_rsp_io_ka(rionic, ib_port, rsp)) {
		_NE(error_8_nordda_connect_nordda_channel_work,
			"Failed io-ka init");
		goto reject;
	}

	/* set target */
	target.common = params;
	target.req = req;
	target.rsp = rsp;

	/* f net */
	rej->reason = 0;
	if (!nvmeibs_net_allocate_target(&nrch->net, &target, rej)) {
		_NT(trace_nordda_connect_nordda_channel_work, "Failed to create target NORDDA QP");
		goto err_io_ka;
	}
	else {
		//nrch->net->priv = (void *)(u64)qp_num;
		nrch->id = qp_num;
	}
	goto out;

err_io_ka:
	nvmeibs_client_rionic_disconnect_ioch(rionic, true);

reject:
	if (rej->reason) {
		_NT(trace_1_nordda_connect_nordda_channel_work, "Reject new connection");
		nvmeibs_send_login_reject(cm_id, rej, req);
	}
	/* we must remember to close the cm_id */
	nvmeib_rdma_destroy_cm(cm_id);

	nvmeib_ref_put(&ib_port->n_port_conns);

	if (nrch) {
		/* bail is needed to be able to free cookies channel */
		if (nrch->cookie_ch) {
			nvmeibs_async_cookie_store_bail_all_ch(nrch->cookie_ch);
			nvmeibs_async_cookie_store_wait_ch(nrch->cookie_ch);
		}
		nrch_rscs_free(nrch);
	}

out:
	if (params)
		kfree(params);
	if (rsp)
		kfree(rsp);
	kfree(w);

	__NFOUT;
}

int nvmeibs_nordda_connect_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	int rv;
	NFIN;

	rv = nvmeibs_client_connect_channel(
		ib_port, cm_id, cl, req, rej, connect_nordda_channel_work);
	_NT(trace_nordda_nvmeibs_nordda_connect_channel, "cl @CL_NAME: @OK_FAIL connect-nordda-channel work to cl-wq",
		cl->name, !rv ? "Added" : "Failed to add");

	NFOUT;
	return rv;
}

/* -------------------------------------------------------------------------- */
/* IO Command Requests  - Alloc and Free                                      */
/* -------------------------------------------------------------------------- */
static inline int nr_io_cmd_md_ib_map(struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_disk_info *di = cmd->nrch->rionic->lionic->disk->di;
	void *virt = NULL;
	struct page *md_page = cmd->bb_map->pages[cmd->bb_map->n_pages - 1];
	int rv = -ENOMEM;
	NFIN;

	if (cmd->md.virt) {
		_NW(warn_nordda_nr_io_cmd_md_ib_map, "OOPS, md already allocated");
		rv = -EINVAL;
		goto out;
	}

	if (!di->metadata) {
		_ND(trace_nordda_nr_io_cmd_md_ib_map, "Nothing to do, disk not formatted with metadata");
		rv = 0;
		goto out;
	}

	/* Metadata pointer (see MPTR), at worse, shall be QWord aligned */
	virt = page_address(md_page);
	
	cmd->md.size = PAGE_SIZE;
	cmd->md.virt = virt;

#if USE_BB_MAP_MR_FOR_MD
	/* Use the same BB MR for the MD to provide overflow protection */
	cmd->md.phys = cmd->bb_map->ioaddr + ((cmd->bb_map->n_pages - 1) << PAGE_SHIFT);
	cmd->md.lkey = cmd->bb_map->lkey;
	cmd->md.rkey = cmd->bb_map->rkey;
#else
	{
		struct nvmeib_dev *dev = cmd->io_cmd_mem.dev;
		dma_addr_t phys;
		phys = ib_dma_map_page(dev->ib_dev, md_page, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
		if (ib_dma_mapping_error(dev->ib_dev, phys)) {
			_NE(error_nordda_nr_io_cmd_md_ib_map, "Fail to DMA map md buffer (size=@SIZE)", (int)PAGE_SIZE);
			goto out;
		}

		cmd->md.phys = phys;
		cmd->md.lkey = nvmeib_get_lkey(dev);
		cmd->md.rkey = nvmeib_get_rkey(dev);
	}
#endif
	
	rv = 0;
	goto out;

out:
    NFOUT;
    return rv;
}

static inline void nr_io_cmd_md_ib_unmap(struct nvmeibs_nr_cmd *cmd)
{
	NFIN;
	/* check that we managed to reach the initialization stage */
	if (!(cmd->nrch && cmd->md.virt))
		goto out;

#if !USE_BB_MAP_MR_FOR_MD
	{
		struct nvmeib_dev *dev = cmd->io_cmd_mem.dev;
		ib_dma_unmap_page(
			dev->ib_dev, cmd->md.phys, cmd->md.size, DMA_BIDIRECTIONAL);
	}
#endif

	cmd->md.virt = NULL;
	cmd->md.phys = 0;
	cmd->md.size = 0;

out:
    NFOUT;
}

static int nr_io_cmd_bb_alloc(struct nvmeibs_disk_info *di, struct nvmeibs_nr_cmd *cmd)
{
	struct nvmeibs_nr_channel * __attribute__((unused)) nrch = cmd->nrch;
	int i;
	int rv = 0;
	unsigned long ts, ts_kzalloc, ts_nmap, ts_dma, ts_end, ts_tmp;
	size_t bb_alloced_size = 0;

	struct device *dev = get_nvme_dma_device(di->dev);

	__NFIN;
	ts = jiffies;
	if (!(cmd->bb_map = kzalloc(sizeof(*cmd->bb_map), GFP_KERNEL))) {
		_NE(error_nordda_nr_io_cmd_bb_alloc, "Fail to allocate IO cmd BB");
		rv = -ENOMEM;
		goto out;
	}
	ts_kzalloc = jiffies - ts;

	ts_tmp = jiffies;
	cmd->bb_map->pd = cmd->io_cmd_mem.dev->pd;
	cmd->bb_map->n_pages = DIV_ROUND_UP(cmd->io_cmd_mem.size, PAGE_SIZE);
	cmd->bb_map->access_flags =
		IB_ACCESS_LOCAL_WRITE |
		IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE;
	cmd->bb_map->ioaddr = 0;
	if ((rv = nvmeib_mem_alloc_n_map(cmd->bb_map)) < 0) {
		_NE(error_1_nordda_nr_io_cmd_bb_alloc, "Fail to map IO cmd BB memory @RV", rv);
		rv = -1;
		goto err_free_bb_map;
	}
	ts_nmap = jiffies - ts_tmp;
	bb_alloced_size = (cmd->bb_map->n_pages * PAGE_SIZE);

	// allocate the prpl on the drive's numa node (first ensure a single page is sufficient)
	ts_tmp = jiffies;
	BUG_ON(nrch->io_cmd_n_pages * sizeof(dma_addr_t) > PAGE_SIZE);
	cmd->bb_prpl_page = alloc_pages_node(dev ? dev->numa_node : NUMA_NO_NODE, GFP_KERNEL, 0);
	if (!cmd->bb_prpl_page) {
		_NE(error_3_nordda_nr_io_cmd_bb_alloc, "Error mapping buffer for dma addresses of bounce buffer");
		rv = -1;
		nvmesh_memmgr_metric_on_alloc_update(s_nordda_bb_pages, bb_alloced_size, false /* success */);
		goto err_unmap;
	}
	bb_alloced_size += PAGE_SIZE;
	nvmesh_memmgr_metric_on_alloc_update(s_nordda_bb_pages, bb_alloced_size, true /* success */);
	cmd->req.buf_addrs = page_address(cmd->bb_prpl_page);

	if (!di->external) {
		BUG_ON(!dev);
		cmd->req.prpl_phys = dma_map_page(dev,
						cmd->bb_prpl_page,
						0,
						PAGE_SIZE,
						DMA_TO_DEVICE);
		if (dma_mapping_error(dev, cmd->req.prpl_phys)) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e2, "Error mapping bounce buffer to disk device");
			rv = -1;
			goto err_free_prpl;
		}
		for (i = 0; i < cmd->bb_map->n_pages; i++) {
			cmd->req.buf_addrs[i] = dma_map_page(dev, cmd->bb_map->pages[i], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, cmd->req.buf_addrs[i])) {
				_NE(error_4_nordda_nr_io_cmd_bb_alloc, "Error mapping buffer for dma addresses of bounce buffer");
				rv = -1;
				goto err_free_bb_phys;
			}
		}
		/* Sync the addresses with the device */
		dma_sync_single_for_device(dev, cmd->req.prpl_phys, PAGE_SIZE, DMA_TO_DEVICE);

		/* set dma addr (to nvme-dev) of md page */
		cmd->req.mtdt_dma_ptr = cmd->req.buf_addrs[cmd->bb_map->n_pages - 1];
	}
	else {
		/* For external disks (SATA/NVMf), we can put the virtual address, and the nvme layer will translate them to a bio */
		for (i = 0; i < cmd->bb_map->n_pages; i++)
			cmd->req.buf_addrs[i] = (unsigned long)page_address(cmd->bb_map->pages[i]);
	}
	ts_dma = jiffies - ts_tmp;

	ts_end = jiffies - ts;
	if (ts_end > NR_ALLOC_TIMEOUT_CMD_BB_ALLOC_WARN)
		_NW(nr_io_cmd_bb_alloc_slow,
			"nrch-req alloc took @LLU (kzalloc @LLU, nmap @LLU, dma @LLU)",
				ts_end, ts_kzalloc, ts_nmap, ts_dma);

	goto out;

err_free_bb_phys:
	for (i--; i >= 0; i--)
		 dma_unmap_page(dev, cmd->req.buf_addrs[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
err_free_prpl:
	dma_unmap_page(dev, cmd->req.prpl_phys, PAGE_SIZE, DMA_TO_DEVICE);
	__free_pages(cmd->bb_prpl_page, 0);
err_unmap:
	nvmeib_mem_unmapn_n_free(cmd->bb_map);
	nvmesh_memmgr_metric_on_free_update(s_nordda_bb_pages, bb_alloced_size);
err_free_bb_map:
	kfree(cmd->bb_map);
	cmd->bb_map = NULL;
out:
	__NFOUT;
	return rv;
}

/* Allocate nordda channel's io-cmds resources */
int nvmeibs_nordda_io_cmds_alloc(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	struct nvmeibs_nr_cmd *cmd;
	int max_rdma_iu;
	struct nvmeib_rdma_iu *rius;
	int max_pages_per_mr = nvmeibs_get_max_pages_in_fmr();
	unsigned long ts;
	unsigned long ts_bb_alloc, ts_map_md, ts_ioctx, ts_kzalloc, ts_end, ts_tmp, ts_req ;
	int i, j, rv = 0;
	void *sges;
	NFIN;

	nrch->io_cmd_n_pages = cl->client_io_cmd_n_pages;

	/* the following test is just a precautions */
	if (nrch->io_cmd_n_pages > max_pages_per_mr) {
		_NT(trace_nordda_nvmeibs_nordda_io_cmds_alloc, "Reducing IO cmd buffer size from @IO_CMD_N_PAGES to @MAX_PAGES_PER_MR",
			nrch->io_cmd_n_pages, max_pages_per_mr);
		nrch->io_cmd_n_pages = max_pages_per_mr;
	}
	if (nrch->io_cmd_n_pages <= 0) {
		_NE(error_nordda_nvmeibs_nordda_io_cmds_alloc, "disk: @DISK_ID_STR nrch: @NRCH_NAME - invalid io_cmd_n_pages value: @IO_CMD_N_PAGES",
		   cl->di->disk_id, nrch->name, nrch->io_cmd_n_pages);
		rv = -EINVAL;
		goto out;
	}
	for (i = 0; i < cl->nrch_ioreq_num; ++i) {
		ts = jiffies;
		cmd = &nrch->io_cmds[i];
		cmd->idx = i;
		cmd->req.use_sg = false;
		cmd->req.arg = cmd;
		cmd->nrch = nrch;
		cmd->io_cmd_mem.dev = P2NV(NR2P(nrch));
		cmd->io_cmd_mem.size = nrch->io_cmd_n_pages << PAGE_SHIFT;
		_ND(nvmeibs_nordda_io_cmds_alloc_d1, "disk: @STR io_cmd_n_pages: @INT mem.size: @INT", cl->di->disk_id,
		   nrch->io_cmd_n_pages, cmd->io_cmd_mem.size);
		if ((rv = nr_io_cmd_bb_alloc(cl->di, cmd)) < 0) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e1, "Allocate io cmd bb ended with error @INT nrch: @STR",
				rv, nrch->name);
			goto out;
		}
		ts_bb_alloc = jiffies - ts;

		ts_tmp = jiffies;
		/* map metadata (last bb) page to nic */
		if ((rv = nr_io_cmd_md_ib_map(cmd)) < 0) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e3, "Allocate io cmd md ended with error @INT nrch: @STR",
				rv, nrch->name);
			goto out;
		}
		ts_map_md = jiffies - ts_tmp;

		ts_tmp = jiffies;
		if (!(cmd->cmd_req = kmem_cache_alloc(NR2P(nrch)->nrch_cmd_req_cache, GFP_KERNEL))) {
			_NE(nvmeibs_nordda_io_cmds_alloc_io_req, "OOM: Failed to allocate io_req for nrch io command");
			rv = -1;
			goto out;
		}
		ts_req = jiffies - ts_tmp;

		ts_tmp = jiffies;
		/* allocate the io_iu (for reply to READ) */
		if (!(cmd->io_iu = nvmeib_alloc_ioctx(P2IB(NR2P(nrch)),
			sizeof(*cmd->io_iu), NVMEIBS_NORDDA_SERVER_MSG_SIZE,
			DMA_TO_DEVICE, NULL))) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e4, "OOM: Fail to allocate io_iu for nrch io command");
			rv = -1;
			goto out;
		}
		ts_ioctx = jiffies - ts_tmp;

		ts_tmp = jiffies;
		/* allocate rdma info buffers - we must have as many rdma_iu as the
		   total messages since we cannot do SG on the read back */
		max_rdma_iu = NVMEIBS_MAX_IO_CHANNEL_MSGS;
		if (!max_rdma_iu ||
			!(rius = kzalloc(max_rdma_iu * sizeof(*rius), GFP_KERNEL))) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e5, "OOM: Fail to allocate rius in io_iu for nrch io command");
			rv = -1;
			goto out;
		}

		if (!(sges = kzalloc(NVMEIB_DEF_SG_PER_WQE * sizeof(struct ib_sge) * max_rdma_iu, GFP_KERNEL))) {
			_NE(nvmeibs_nordda_io_cmds_alloc_e6, "OOM: Fail to allocate SG in io_iu for nrch io command");
			rv = -1;
			goto out;
		}

		for (j = 0; j < max_rdma_iu; ++j) {
			rius[j].sge  = sges + (NVMEIB_DEF_SG_PER_WQE * sizeof(struct ib_sge) * j);
		}
		cmd->io_iu->opcode = NVMEIB_IU_PRIV;
		cmd->io_iu->max_rdma_iu = max_rdma_iu;
		cmd->io_iu->rius = rius;
		cmd->io_iu->index = i;
		cmd->io_iu->priv = cmd;
		ts_kzalloc = jiffies - ts_tmp;

		ts_end = jiffies - ts;
		/* catch stalling on JT's cluster */
		if (ts_end > NR_ALLOC_TIMEOUT_IO_CMD_BB_ALLOC_WARN)
			_NW(nvmeibs_nordda_io_cmds_alloc_w0,
				"nrch-req alloc took @LLU (bb_alloc @LLU, map_md @LLU, req @LLU, ioctx @LLU, kzalloc @LLU)",
				 ts_end, ts_bb_alloc, ts_map_md, ts_req, ts_ioctx, ts_kzalloc);
	}
out:
	NFOUT;
	return rv;
}

int nvmeibs_nordda_fill_config_alloc_nr_net_rsp(struct nvmeibs_nr_channel *nrch,
	struct volume_server_config_alloc_nr_net_rsp *rsp)
{
	struct nvmeibs_nr_cmd *cmd;
	struct volume_server_client_req_io_area *ioa;
	struct nvmeibs_client *cl = NR2C(nrch);
	struct nvmeibs_disk_info *di = cl->di;
	struct nvmeibs_dev *dev = NR2P(nrch)->nis_dev;
	struct nvmeib_remote_access_info jmdc_rai;
	int i, rv = 0;
	NFIN;

	for (i = 0; i < cl->nrch_ioreq_num; ++i) {
		cmd = &nrch->io_cmds[i];
		ioa = &rsp->a[i];
		ioa->raddr = cpu_to_be64(cmd->bb_map->ioaddr);
		ioa->rn_pages = cpu_to_be32(cmd->bb_map->n_pages);
		ioa->rkey = cpu_to_be32(cmd->bb_map->rkey);

		ioa->md_desc.raddr = cpu_to_be64(cmd->md.phys);
		ioa->md_desc.size = cpu_to_be32(cmd->md.size);
		ioa->md_desc.rkey = cpu_to_be32(cmd->md.rkey);

		_ND(nvmeibs_nordda_fill_config_alloc_nr_net_rsp_d1, "ioa=@INT, raddr=@_X, rn_pages=@UINT, rkey=@INT32_HEX",
		   i, ioa->raddr, be32_to_cpu(ioa->rn_pages), be32_to_cpu(ioa->rkey));

		_ND(nvmeibs_nordda_fill_config_alloc_nr_net_rsp_d2, "ioa->md_desc[@INT]: raddr=@_X, size=@UINT, rkey=@INT32_HEX",
		   i, be64_to_cpu(ioa->md_desc.raddr),
			be32_to_cpu(ioa->md_desc.size), be32_to_cpu(ioa->md_desc.rkey));
	}

	if (!di->metadata) {
		_NT(trace_3_nordda_nvmeibs_nordda_fill_config_alloc_nr_net_rsp,
		    "Disk @DISK_NAME has no metadata - zeroing jmdc_desc",
			nrch->rionic->lionic->disk->di->disk_id);
		rsp->jmdc_desc.raddr = 0;
		rsp->jmdc_desc.len = 0;
		rsp->jmdc_desc.rkey = 0;
		goto out;
	}

	if (cl->jrnl_rng == NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NT(trace_2_nordda_nvmeibs_nordda_fill_config_alloc_nr_net_rsp,
		    "Journal range not allocated for disk @DISK_NAME - zeroing jmdc_desc",
			nrch->rionic->lionic->disk->di->disk_id);
		rsp->jmdc_desc.raddr = 0;
		rsp->jmdc_desc.len = 0;
		rsp->jmdc_desc.rkey = 0;
		goto out;
	}

	if (IS_ERR(nrch->jrange_handle = nvmeibs_serjio_get_jrange_handle(
				di, cl->cid, cl->jrnl_rng, dev, &nrch->jmdc_map_handle)))
	{
		_NT(trace_1_nordda_nvmeibs_nordda_fill_config_alloc_nr_net_rsp, "Failed (@RV) getting handle for jrange @JRNL_RNG_IDX", PTR_ERR(nrch->jrange_handle), cl->jrnl_rng);
		rv = -1;
		goto out;
	}

	if ((rv = nvmeibs_serjio_get_jrange_jmdc_rai(
		nrch->jrange_handle, nrch->jmdc_map_handle, &jmdc_rai)) < 0)
	{
		_NT(trace_4_nordda_nvmeibs_nordda_fill_config_alloc_nr_net_rsp,
		    "Failed (@RV) getting jmdc rai for jrange @JRNL_RNG_IDX", rv, cl->jrnl_rng);
		goto out;
	}

	rsp->jmdc_desc.raddr = cpu_to_be64(jmdc_rai.raddr);
	rsp->jmdc_desc.len = cpu_to_be32(jmdc_rai.len);
	rsp->jmdc_desc.rkey = cpu_to_be32(jmdc_rai.rkey);

	_NT(trace_nordda_nvmeibs_nordda_fill_config_alloc_nr_net_rsp,
	    "jrange @JRNL_RNG_IDX handle @JRANGE_HANDLE jmdc_map_handle @HANDLE_PTR jmdc-rai={raddr=@RADDR, rkey=@RKEY}",
		cl->jrnl_rng, nrch->jrange_handle, nrch->jmdc_map_handle, jmdc_rai.raddr, jmdc_rai.rkey);

out:
	NFOUT;
	return rv;
}

static void nr_io_cmds_mr_unmap_n_free(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeibs_client *cl = NR2C(nrch);
	int i;
	struct nvmeibs_nr_cmd *cmd;
	struct device *dev;
	unsigned long max_iter_s, max_iter=0, duration_s=jiffies;

	__NFIN;

	dev = get_nvme_dma_device(cl->di->dev);

	for (i = 0; i < cl->nrch_ioreq_num; ++i) {
		max_iter_s = jiffies;
		cmd = &nrch->io_cmds[i];
		if (cmd->bb_map) {
			int j;

			if (!cl->di->external) {
				BUG_ON(!dev);

				/* unmap md (last bb) page from nic */
				nr_io_cmd_md_ib_unmap(cmd);

				for (j = 0; j < cmd->bb_map->n_pages; j++)
					dma_unmap_page(dev, cmd->req.buf_addrs[j], PAGE_SIZE, DMA_BIDIRECTIONAL);
			}
			dma_unmap_page(dev, cmd->req.prpl_phys, PAGE_SIZE, DMA_TO_DEVICE);
			__free_pages(cmd->bb_prpl_page, 0);
			nvmeib_mem_unmapn_n_free(cmd->bb_map);
			nvmesh_memmgr_metric_on_free_update(s_nordda_bb_pages, (cmd->bb_map->n_pages * PAGE_SIZE) + PAGE_SIZE);
			kfree(cmd->bb_map);
			cmd->bb_map = NULL;
		}
		max_iter = max(max_iter, (jiffies - max_iter_s));
		if (((jiffies - max_iter_s) / HZ) >= NVMEIBS_UNMAP_REQUEST_WARN_SECONDS) {
			WARN_KNOWN_ONCE(1, 542);
			/* cond resched? */
		}
	}
	_NT(nr_io_cmds_mr_unmap_n_free_summary, "full runtime - @DIFF, max iter - @DIFF", (jiffies - duration_s) / HZ , max_iter / HZ);
	__NFOUT;
}

static void nr_io_cmds_free(struct nvmeibs_nr_channel *nrch)
{
	int i;
	struct nvmeibs_nr_cmd *cmd;
	struct nvmeibs_client *cl = NR2C(nrch);

	__NFIN;
	for (i = 0; i < cl->nrch_ioreq_num; ++i) {
		cmd = &nrch->io_cmds[i];
		if (cmd->cmd_req) {
			kmem_cache_free(NR2P(nrch)->nrch_cmd_req_cache, cmd->cmd_req);
			cmd->cmd_req = NULL;
		}
		if (cmd->io_iu) {
			/* free rdma iu */
			if (cmd->io_iu->rius) {
				kfree(cmd->io_iu->rius[0].sge);
				kfree(cmd->io_iu->rius);
			}
			/* free attached iu */
			nvmeib_free_ioctx(P2IB(NR2P(nrch)), cmd->io_iu,
				NVMEIBS_NORDDA_SERVER_MSG_SIZE, DMA_TO_DEVICE);
			cmd->io_iu = NULL;
		}

		/* clear counters */

#ifdef NVMEIBS_NR_CATCH_IO_DBL_CB
		atomic64_set(&cmd->n_submit, 0);
		atomic64_set(&cmd->n_cb, 0);
		memset(&cmd->cb_ents, 0, sizeof(cmd->cb_ents));
#endif

		atomic_set(&cmd->n_send_rsp_io, 0);
		atomic_set(&cmd->n_send_rsp_io_comp, 0);
		cmd->version = 0;
		cmd->ch_version = 0;
	}
	__NFOUT;
}




static void unref_pending_requests(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeibs_net *net = nrch->net;
	struct list_head *l = &net->pending_received_msgs;
	struct nvmeib_iu *recv_ioctx, *recv_ioctx_t;
	int num_pending_received_msgs = 0;
	NFIN;

	// We're decreasing the refcount per each pending message as the associated
	// message will never be processed as the QP is not processing any more completions.
	list_for_each_entry_safe(recv_ioctx, recv_ioctx_t, l, free_tx_n) {
		struct volume_client_req *req = recv_ioctx->buf;
		num_pending_received_msgs++;
		cmd_ended(nrch, req->hdr.tag);
	}

	if (num_pending_received_msgs)
		_NW(warn_nordda_unref_pending_requests, "Fixed reference count for @NUM_PENDING_RECEIVED_MSGS pending messages", num_pending_received_msgs);

	NFOUT;
}

/* -------------------------------------------------------------------------- */
/* Release/Disconnect - carried out by the net                                */
/* -------------------------------------------------------------------------- */
static void nordda_bn_handler(void *context)
{
	struct nvmeibs_nr_channel *nrch = context;
	__NFIN;

	_NT(trace_nordda_nordda_bn_handler, "nrch @NRCH_NAME (@NRCH)", nrch->name, nrch);
	BUG_ON(nrch->net->state != QP_RELEASING);

	nvmeibs_client_rionic_disconnect_ioch(nrch->rionic, true);

	/* This can be called at this point because no new incoming requests can be
	 appended to pending_received_msgs as the QP is already disconnected.
	 The pending_received_msgs list will be cleared later by release_work().*/
	_NT(trace_1_nordda_nordda_bn_handler, "unref pending requests");
	unref_pending_requests(nrch);

	/* bail async cookies - all in queue are canceled, no new allowed */
	_NT(trace_2_nordda_nordda_bn_handler, "bail async cookies");
	nvmeibs_async_cookie_store_bail_all_ch(nrch->cookie_ch);

	/* now, that we have cmd-ended the ones in
	   pending-requests and async-cookies,
	   wait for all disk's completion.
	   on read-comp disk writes to BB */
	_NT(trace_3_nordda_nordda_bn_handler, "wait disk io-cmds");
	wait_for_all_underway(nrch);

	_NT(trace_s_nordda_nordda_bn_handler,
		"Putting Disk @DISK_NAME jrange handle @JRANGE_HANDLE jmdc_map_handle @HANDLE_PTR",
		nrch->rionic->lionic->disk->di->disk_id, nrch->jrange_handle, nrch->jmdc_map_handle);
	nvmeibs_serjio_put_jrange_handle(nrch->jrange_handle, nrch->jmdc_map_handle);
	nrch->jrange_handle = NULL;
	nrch->jmdc_map_handle = NULL;

	/* wait for all async cookie completion, bail already happened */
	_NT(trace_4_nordda_nordda_bn_handler, "wait async cookies");
	nvmeibs_async_cookie_store_wait_ch(nrch->cookie_ch);

	/* free resources allocated on login request */
	_NT(trace_6_nordda_nordda_bn_handler, "free rsrcs");
	nrch_rscs_free(nrch);

	/* free resources allocated on alloc-nr-net req */
	_NT(trace_5_nordda_nordda_bn_handler, "unmap and free io-cmds");
	nr_io_cmds_mr_unmap_n_free(nrch);
	nr_io_cmds_free(nrch);

	/* send ioch-drained */
	_NT(trace_7_nordda_nordda_bn_handler, "cs-gid: sent=@LLU, curr=@LLU",
		nrch->cs_gid_last_sent, nrch->cs_gid);
	if (nrch->id != -1 && nrch->cs_gid_last_sent != nrch->cs_gid) {
		nvmeibs_client_send_ioch_drained(NR2C(nrch), false,
										 nrch->rionic, nrch->id, nrch->cs_gid);
		nrch->cs_gid_last_sent = nrch->cs_gid;
	}

	/* make channel reuseable */
	nrch->id = -1;

	__NFOUT;
}

static void nordda_ad_handler(void *context)
{
	struct nvmeibs_nr_channel *nrch = context;
	__NFIN;

	_NT(trace_nordda_nordda_ad_handler, "nrch @NRCH_NAME (@NRCH)", nrch->name, nrch);
	BUG_ON(nrch->net->state != QP_RELEASING);

	if (nvmeibs_use_pcpu_cq) {
		if (!nrch->net || !nrch->net->dev_cq || !nrch->net->srq_info) {
			_NE(trace_err_nordda_nordda_ad_handler, "nrch @NRCH_NAME (@NRCH), cant post recv...", nrch->name, nrch);
		}
		else {
			/* free recv-iu(s) that we have stashed till send-comp of io-rsp.
			   not expecting any as we processsend-comp even when nrch is dying */
			rxiu_drain(nrch);
		}
	}

	__NFIN;
}

static void nordda_after_destroy_ib_handler(void *context)
{
	struct nvmeibs_nr_channel *nrch = context;
	__NFIN;

	_NT(trace_nordda_after_destroy_ib_handler, "nrch @NRCH_NAME (@NRCH)", nrch->name, nrch);
	BUG_ON(nrch->net->state != QP_RELEASING);

	nvmeibs_nr_lat_meas_free_siw_md(nrch);

	if (nrch->scq_wcs) {
		kfree(nrch->scq_wcs);
		nrch->scq_wcs = NULL;
	}
	if (nrch->rcq_wcs) {
		kfree(nrch->rcq_wcs);
		nrch->rcq_wcs = NULL;
	}

	__NFIN;
}

/*
 * Per device (shared) CQs
 */
static struct nvmeib_iu *rxiu_pop_(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeib_iu *recv_ioctx = NULL;
	__NFIN;

	if (list_empty(&nrch->rxiu_list)) {
		_NE(rxiu_pop__e1, "nrch @PTR, OOPS rxiu_list is empty (@INT)",
		   nrch, nrch->n_rxiu);
		WARN_ON(1);
	}
	else {
		recv_ioctx = list_first_entry(&nrch->rxiu_list,
						struct nvmeib_iu, free_tx_n);
		list_del_init(&recv_ioctx->free_tx_n);
		nrch->n_rxiu--;
	}

	__NFOUT;
	return recv_ioctx;
}

struct nvmeib_iu *rxiu_pop(struct nvmeibs_nr_channel *nrch)
{
	struct nvmeib_iu *recv_ioctx = NULL;
	unsigned long flags;
	__NFIN;

	spin_lock_irqsave(&nrch->spinlock, flags);
	recv_ioctx = rxiu_pop_(nrch);
	spin_unlock_irqrestore(&nrch->spinlock, flags);

	__NFOUT;
	return recv_ioctx;
}

void rxiu_drain(struct nvmeibs_nr_channel *nrch)
{
	u32 n_reqs = NR2C(nrch)->nrch_ioreq_num;
	struct nvmeib_iu *recv_ioctx;
	unsigned long flags;
	int n = 0;
	__NFIN;

	/* expecting empty list as we process send-comp even when nrch is dying */
	spin_lock_irqsave(&nrch->spinlock, flags);
	nrch->rxiu_dying = 1;
	if (!list_empty(&nrch->rxiu_list)) {
		_NW(rxiu_drain_w1, "Unexpected, found @INT stashed rxiu "
		   "(n_req=@INT, tot=@LLU), [@INT, @INT]",
		   nrch->n_rxiu, n_reqs, nrch->n_rxiu_tot,
		   nvmeibs_use_pcpu_cq, nvmeibs_nr_post_recv_on_send_comp);
		WARN_ON(1);
		BUG_ON(nrch->n_rxiu > n_reqs);

		/* drain & post-recv */
		while (!list_empty(&nrch->rxiu_list)) {
			recv_ioctx = rxiu_pop_(nrch);
			post_recv_iu(nrch, recv_ioctx);
			n++;
		}

		if (n != nrch->n_rxiu) {
			_NW(rxiu_drain_w2, "Inconsistent list @INT vs. @UINT",
			   n, nrch->n_rxiu);
			WARN_ON(1);
		}
	}
	spin_unlock_irqrestore(&nrch->spinlock, flags);

	__NFOUT;
}

/* we use the nrch spinlock but as CQ is percpu,
   it is not expected to bounce chacheline */
static int rxiu_push(struct nvmeibs_nr_channel *nrch, struct nvmeib_iu *recv_ioctx)
{
	unsigned long flags;
	int rv = -1;
	__NFIN;

	if (recv_ioctx->defer) {
		_NE(rxiu_push_e1, "nrch @PTR, recv_ioctx is still defered", nrch);
		goto out;
	}

	spin_lock_irqsave(&nrch->spinlock, flags);
	if (nrch->rxiu_dying) {
		_NE(rxiu_push_e2, "nrch @PTR, rxiu already dying", nrch);
	}
	else if (!list_empty(&recv_ioctx->free_tx_n)) {
		_NE(rxiu_push_e3, "nrch @PTR, recv_ioctx=@PTR, already linked", nrch, recv_ioctx);
	}
	/* after sending io-rsp[i], client may issue another io-req[i] which
	   its recv-comp may arrive before the send-comp of prev io-rsp[i] */
	#if 0
	else if (nrch->n_rxiu == NR2C(nrch)->nrch_ioreq_num) {
		_NE(rxiu_push_e4, "nrch @PTR, rxiu overflow @UINT", nrch, nrch->n_rxiu);
	}
	#endif
	else {
		list_add_tail(&recv_ioctx->free_tx_n, &nrch->rxiu_list);
		nrch->n_rxiu++;
		nrch->n_rxiu_tot++;
		rv = 0;
	}
	spin_unlock_irqrestore(&nrch->spinlock, flags);

out:
	__NFOUT;
	return rv;
}

void nrch_send_rsp_and_post_recv(struct nvmeibs_nr_channel *nrch,
	u8 opcode, u64 tag, u16 version_tag, struct nvmeib_iu *send_ioctx, void *p, int len,
	int wr_opcode, u16 wr_version, struct nvmeib_iu *recv_ioctx)
{
	bool pushed = false;
	int rv;
	__NFIN;

	if (wr_opcode == NVMEIB_REPLY_IO_W_RDMA || wr_opcode == NVMEIB_REPLY_GEN_W_RDMA)
		BUG_ON(send_ioctx->index != nordda_tag_decode_index(be64_to_cpu(tag)));

	/* push before rsp's send-comp may arrive */
	if (nvmeibs_nr_post_recv_on_send_comp && recv_ioctx) {
		_ND(nrch_send_rsp_and_post_recv_d1, "nrch @PTR, Attempt rxiu push recv_ioctx=@PTR is-linked=@INT (@STR)",
		   nrch, recv_ioctx, !list_empty(&recv_ioctx->free_tx_n),
		   nvmeib_wr_opcode_str(wr_opcode));

		if (rxiu_push(nrch, recv_ioctx)) {
			_NE(nrch_send_rsp_and_post_recv_e1, "nrch @PTR, Failed rxiu push, fallback to post-SRQ (@STR)",
			   nrch, nvmeib_wr_opcode_str(wr_opcode));
			WARN_ON(1);
		}
		else {
			pushed = true;
		}
	}

	rv = nrch_send_rsp(NR2C(nrch), nrch,
					   opcode, tag, version_tag, send_ioctx, p, len, wr_opcode, wr_version);

	/* if send failed, there will be no send-comp to pop this pushed rxiu... */
	if (nvmeibs_nr_post_recv_on_send_comp && recv_ioctx) {
		if (rv && pushed) {
			struct nvmeib_iu *other;
			other = rxiu_pop(nrch);
			_NE(nrch_send_rsp_and_post_recv_e2, "nrch @PTR, Failed send-rsp, fallback to post-SRQ (@STR), "
			   "rxiu=@PTR --> other_rxiu=@PTR",
			   nrch, nvmeib_wr_opcode_str(wr_opcode), recv_ioctx, other);
			recv_ioctx = other;
			pushed = false;
		}
	}

	if (!pushed)
		post_recv_iu(nrch, recv_ioctx);

	__NFOUT;
}

static void nordda_send_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_nr_channel *nrch = ctx;
	struct nvmeibs_client *cl = NR2C(nrch);

	__NFIN;
	if (cl->dismissed)
		_NT(trace_nordda_nordda_send_comp_h, "Allow processing of drain-sq wr");
	process_send_completion(nrch, wcs, NULL);
	__NFOUT;
}

static void nordda_recv_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_nr_channel *nrch = ctx;
	struct nvmeibs_net *net = nrch->net;
	struct nvmeibs_client *cl = NR2C(nrch);
	int rv;

	__NFIN;
	nvmeibs_net_dec_recv(net, 1);
	if (nvmeib_opcode_from_wc(wcs) == NVMEIB_DRAIN_QUEUE) //TODO: Remove this
		nvmeibs_rq_drain_comp(net, wcs);
	else if (!cl->dismissed) {
		if ((rv = process_recv_completion(nrch, wcs, false))) {
			_ND(trace_nordda_nordda_recv_comp_h, "process_recv_completion failed (@RV)", rv);
		}
	}
	else {
		u32 index = nvmeib_idx_from_wc(wcs);
		struct nvmeib_iu *recv_ioctx = get_recv_iu(nrch, index);
		if (recv_ioctx)
			post_recv_iu(nrch, recv_ioctx);
		else
			_NE(error_nordda_nordda_recv_comp_h, "Got null recv_ioctx");
	}
	__NFOUT;
}
#else	// defined(BLKDEV_SIMULATOR)
	#include "nvmeibc_block.h"
	#include "nvmeibs_nordda_sim_shared.h"
	#include "block/unitest/nvmeibc_simu_disk.h"
#endif /* !BLKDEV_SIMULATOR */

/* Simulator-server shared code */

/* Begin simulator - server shared code */
static DECLARE_SERJIO_RNG_CB_FN(handle_get_uuid_jour_cb)
{
	struct handle_get_uuid_jour_cb_param *param = ctx;
	const struct nvmeib_gen_cmd_param *gen_param = param->gen_param;
	union nvmeib_gen_cmd_rsp *rsp = param->gen_rsp;
	int rv = 0;
	unsigned int i;
	unsigned dest_jmdc_offset = 0;

	/* Unused */
	(void)n_dirty_ents_in_seg;
	(void)n_abnd_ents_in_seg;

	BUG_ON(nvmeib_uuid_cmp(client_uuid, gen_param->uj.client_uuid) != 0);
	rsp->uj.jour.rng_id = range_idx;
	rsp->uj.jour.rng_gen_id = gen_id;
	rsp->uj.jour.rng_slba = start_clsect;
	rsp->uj.jour.rng_nlba = size_clsect;
	rsp->uj.jour.client_uuid = client_uuid;
	rsp->uj.ent_md_len = ent_md_sz;
	bitmap_copy(rsp->uj.jour.dirty_ents_bitmap, dirty_ents_in_seg_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	bitmap_copy(rsp->uj.jour.abnd_ents_bitmap, abnd_ents_in_seg_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);

	rsp->uj.jour.binje = range_binje;
	rsp->uj.jour.n_ents = num_entries;
	rsp->uj.jour.rng_blk = range_binje * num_entries;

	_ND(trace_handle_get_uuid_jour_cb, "Client: @CLIENT_UUID - Range: @JRNL_RNG_IDX"
	" GenID: @JRNL_RNG_GEN_ID Dirty: " NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE
	" Abandoned: " NVMEIB_EC_JOURNAL_ENTRIES_STATE_TRACE, &client_uuid, range_idx,
	 gen_id, dirty_ents_in_seg_bmp, abnd_ents_in_seg_bmp);

	for (i = 0; i < num_entries && dest_jmdc_offset < gen_param->uj.jmdc_dest.local.size; i++) {
		union jblock_md *jmdc_ent = (*get_jmdc_ent_fn)(rng_handle, i);
		unsigned jmdc_ent_sz = range_binje * sizeof(*jmdc_ent);

		nvmeib_buffer_copy_from_buffer_ext(
			(struct nvmeib_buffer *)&gen_param->uj.jmdc_dest.local,
			jmdc_ent, jmdc_ent_sz, dest_jmdc_offset);
		dest_jmdc_offset += jmdc_ent_sz;
	}

	if (i < num_entries) {
		_NE(error_nordda_handle_get_uuid_jour_cb,
		    "jmdc rai len too small @JMDC_SIZE, only @N_ENTS copied (@JMDC_SIZE)",
			gen_param->uj.jmdc_dest.local.size, i, dest_jmdc_offset);
		rv = -ENOMEM;
		goto out;
	}

	param->jmdc_sz = rsp->uj.jmdc_len = dest_jmdc_offset;

	if (ent_md_sz > gen_param->uj.ent_md_dest.local.size) {
		_NE(error_1_nordda_handle_get_uuid_jour_cb, "ent_md rai len too small @ENT_MD_SZ < @ENT_MD_SZ",
			gen_param->uj.ent_md_dest.local.size, ent_md_sz);
		rv = -ENOMEM;
		goto out;
	}

	nvmeib_buffer_copy_from_buffer((struct nvmeib_buffer *)&gen_param->uj.ent_md_dest.local, ent_md, ent_md_sz);
	param->ent_md_sz = ent_md_sz;

out:
	return rv;
}

/**
 * Let Serjio fill UUID's jmdc and its desc to @cmd's first BB and @rsp, respectively.
 */
int nvmeibs_gen_cmd_handle_get_uuid_jour(struct nvmeibs_disk_info *di, struct handle_get_uuid_jour_cb_param *param) {
	int rv = 0;

	NFIN;
	rv = nvmeibs_serjio_call_for_client_range(di, param->gen_param->uj.client_uuid, param->gen_param->uj.binje,
											  param->gen_param->uj.sgmnt_uuid, handle_get_uuid_jour_cb, param);
	if (rv < 0) {
		if (rv == -ENOENT) {
			_NT(trace_nordda_nvmeibs_gen_cmd_handle_get_uuid_jour,
				"No jmdc for uuid @CLIENT_UUID", &param->gen_param->uj.client_uuid);
			param->gen_rsp->uj.jour.rng_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;
			param->gen_rsp->uj.jour.binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
			param->jmdc_sz = 0;
			param->ent_md_sz = 0;
			rv = 0;
			goto out;
		} else {
			_NT(trace_1_nordda_nvmeibs_gen_cmd_handle_get_uuid_jour,
				"Fail to get uuid @CLIENT_UUID journal, rv=@RV",
				&param->gen_param->uj.client_uuid, rv);
			rv = NVMEIBS_IO_RSP_ERR_UUID_JOUR;
		}
		goto out;
	}

	memcpy(param->gen_rsp->uj.jour.serjio_boot_id, nvmeibs_serjio_get_boot_id(di), NVMEIB_GID_STR_MAX);
out:
	NFOUT;
	return rv;
}

