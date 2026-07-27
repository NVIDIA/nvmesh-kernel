#ifndef NVMEIBC_NR_LAT_MEAS_H
#define NVMEIBC_NR_LAT_MEAS_H

#if NVMEIBC_NR_LAT_MEAS
enum nvmeibc_nr_lat_meas_nrch_lat_type {
	C_NRCH_LAT_E2E = 0, 		/* End-to-End */
	C_NRCH_LAT_SQ_PEND, 	/* Send Queue Pending (SIW Only) */
	C_NRCH_LAT_ACK_TIME, 	/* Time to ACK Send (SIW Only) */
	C_NRCH_LAT_SQ_POST2COMP,/* Time between ib_post_send and Send Completion */
	C_NRCH_LAT_TGT_TIME,    /* Time processed by target (ACK to Recv) */
	C_NRCH_LAT_CQ_PEND, 	/* Completion Queue Pending (SIW Only) */
	C_NRCH_LAT_MAX,
};

static inline const char *nvmeibc_nr_lat_meas_lat_type_name(enum nvmeibc_nr_lat_meas_nrch_lat_type lat_type)
{
	switch (lat_type) {
	case C_NRCH_LAT_E2E: return "E2E";
	case C_NRCH_LAT_SQ_PEND: return "SQ_Pend";
	case C_NRCH_LAT_ACK_TIME: return "Ack";
	case C_NRCH_LAT_SQ_POST2COMP: return "SQ Post->Comp";
	case C_NRCH_LAT_TGT_TIME: return "Target";
	case C_NRCH_LAT_CQ_PEND: return "CQ_Pend";
	default: return "INVALID";
	}
}

static inline bool nvmeibc_nr_lat_meas_is_nrch_lat_meas_siw_only(enum nvmeibc_nr_lat_meas_nrch_lat_type type) {
	switch (type) {
		/* These rely on SIW probes which aren't available in mlx code */
		case C_NRCH_LAT_SQ_PEND:
		case C_NRCH_LAT_ACK_TIME:
		case C_NRCH_LAT_CQ_PEND:
			return true;
		default:
			break;
	}
	return false;
}

static inline bool nvmeibc_nr_lat_meas_nrch_lat_meas_needs_send_comp(enum nvmeibc_nr_lat_meas_nrch_lat_type type) {
	switch (type) {
		case C_NRCH_LAT_SQ_PEND:
		case C_NRCH_LAT_ACK_TIME:
		case C_NRCH_LAT_SQ_POST2COMP:
		case C_NRCH_LAT_TGT_TIME:
			return true;
		default:
			break;
	}
	return false;
}

struct nvmeibc_nr_lat_meas_nrch_lat_data {
	u32 min_usec;
	u32 max_usec;
	u64 num_meas;
	u64 tot_usec;
};

struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data {
	struct nvmeibc_nr_lat_meas_nrch_lat_data lat_data[C_NRCH_LAT_MAX];
	u64 tx_cpu_cnt[NVMEIB_DFLT_MAX_CPUS];
	u64 rx_cpu_cnt[NVMEIB_DFLT_MAX_CPUS];
	u64 rx_queue_cnt[NVMEIB_DFLT_MAX_CPUS];
	u32 rx_skb_hash;
};

struct nvmeibc_nr_lat_meas_nrch_req_meas {
	enum nvmeibc_disk_command_type dcmd_type;
	ktime_t sq_post_time;
	ktime_t recv_comp_time;
	ktime_t send_comp_time;
	ktime_t sent_time;
	ktime_t ack_time;
	ktime_t recv_time;
	ktime_t rcq_poll_time;
	u16 tx_cpu;
	u16 rx_cpu;
	u16 rx_queue;
	u32 rx_skb_hash;
};

static inline void nvmeibc_nr_lat_meas_clear_nrch_pcpu_data(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data)
{
	int cpu;
	/* Clear latency counters */
	for_each_possible_cpu(cpu) {
		struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data *pcpu_lat_data = (per_cpu_ptr(per_cpu_lat_data, cpu));
		memset(pcpu_lat_data, 0, sizeof(*pcpu_lat_data));
	}
}

static inline bool nvmeibc_nr_lat_meas_alloc_wc_md(bool alloc_siw_wc_md,
							struct ib_wc *wc_s, struct ib_wc *wc_r, struct ib_wc *wc_m,
							int n_wc_s, int n_wc_r, int n_wc_m,
							void **wc_md_s, void **wc_md_r, void **wc_md_m) 
{
	if (alloc_siw_wc_md) {
		/* Fill in the MD pointers in the wr_id field */
		if (!(*wc_md_s = nvmeib_rdma_alloc_siw_wc_md(n_wc_s, wc_s, GFP_KERNEL)) ||
			!(*wc_md_r = nvmeib_rdma_alloc_siw_wc_md(n_wc_r, wc_r, GFP_KERNEL)) ||
			!(*wc_md_m = nvmeib_rdma_alloc_siw_wc_md(n_wc_m, wc_m, GFP_KERNEL))) {
			
			return false;
		}
	}
	return true;
}

static inline void nvmeibc_nr_lat_meas_init_req(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas,
						enum nvmeibc_disk_command_type dcmd_type)
{
	memset(lat_meas, 0, sizeof(*lat_meas));
	lat_meas->dcmd_type = dcmd_type;
}

static inline void nvmeibc_nr_lat_meas_sq_post(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas)
{
	lat_meas->sq_post_time = nvmeib_public_ktime_get();
}

static inline void nvmeibc_nr_lat_meas_send_comp(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas,
						const struct ib_wc *wc)
{
	if (!nvmeib_rdma_siw_wc_get_tx_md(wc, 
		&lat_meas->sq_post_time,
		&lat_meas->sent_time,
		&lat_meas->ack_time,
		&lat_meas->tx_cpu)) {

		lat_meas->tx_cpu = smp_processor_id();
	}
}

static inline void nvmeibc_nr_lat_meas_recv_comp(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas,
						 const struct ib_wc *wc)
{
	ktime_t start_recv_time; /* Not used in stats */
	
	if (!nvmeib_rdma_siw_wc_get_rx_md(wc,
		&start_recv_time,
		&lat_meas->recv_time,
		&lat_meas->rcq_poll_time,
		&lat_meas->rx_cpu,
		&lat_meas->rx_queue,
		&lat_meas->rx_skb_hash)) {

		lat_meas->recv_time = nvmeib_public_ktime_get();
		lat_meas->rcq_poll_time = nvmeib_public_ktime_get();
		lat_meas->rx_cpu = smp_processor_id();
	}
}

static inline void nvmeibc_nr_lat_meas_recv_comp_process(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas)
{
	lat_meas->recv_comp_time = nvmeib_public_ktime_get();
}

static inline void nvmeibc_nr_lat_meas_update_nrch_pcpu_data(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data,
	struct nvmeibc_nr_lat_meas_nrch_req_meas *req_lat_meas, bool got_send_comp, bool is_siw)
{
	if (req_lat_meas->dcmd_type == NVMEIBC_DISK_CMD_IO) {
		/* Update IO latency measurements */
		struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data *cpu_lat_data;
		unsigned long flags;
		u32 this_usec;
		int meas_type;
		
		local_irq_save(flags);
		cpu_lat_data = this_cpu_ptr(per_cpu_lat_data);
		/* Optimizer will unroll this loop, it just makes the code smaller */
		for (meas_type = 0; meas_type < C_NRCH_LAT_MAX; meas_type++) {
			if (!is_siw && nvmeibc_nr_lat_meas_is_nrch_lat_meas_siw_only(meas_type))
				continue;
			if (!got_send_comp && nvmeibc_nr_lat_meas_nrch_lat_meas_needs_send_comp(meas_type))
				continue;
			switch (meas_type) {
				case C_NRCH_LAT_E2E:
					this_usec = ktime_to_us(ktime_sub(req_lat_meas->recv_comp_time, req_lat_meas->sq_post_time));
					break;
				case C_NRCH_LAT_SQ_PEND:
					this_usec = ktime_to_us(ktime_sub(req_lat_meas->sent_time, req_lat_meas->sq_post_time));
					break;
				case C_NRCH_LAT_ACK_TIME:
					this_usec = ktime_to_us(ktime_sub(req_lat_meas->ack_time, req_lat_meas->sent_time));
					break;
				case C_NRCH_LAT_SQ_POST2COMP:
					this_usec = ktime_to_us(ktime_sub(req_lat_meas->send_comp_time, req_lat_meas->sq_post_time));
					break;
				case C_NRCH_LAT_TGT_TIME:
					if (is_siw)
						this_usec = ktime_to_us(ktime_sub(req_lat_meas->recv_time, req_lat_meas->ack_time));
					else
						this_usec = ktime_to_us(ktime_sub(req_lat_meas->recv_comp_time, req_lat_meas->send_comp_time));
					break;
				case C_NRCH_LAT_CQ_PEND:
					this_usec = ktime_to_us(ktime_sub(req_lat_meas->rcq_poll_time, req_lat_meas->recv_time));
					break;
				default:
					BUG();
			}
			
			cpu_lat_data->lat_data[meas_type].num_meas++;
			cpu_lat_data->lat_data[meas_type].tot_usec += this_usec;
			cpu_lat_data->lat_data[meas_type].min_usec = min(this_usec, cpu_lat_data->lat_data[meas_type].min_usec);
			cpu_lat_data->lat_data[meas_type].max_usec = max(this_usec, cpu_lat_data->lat_data[meas_type].max_usec);
		}
		if (req_lat_meas->tx_cpu >= 0 && req_lat_meas->tx_cpu < NVMEIB_DFLT_MAX_CPUS)
			cpu_lat_data->tx_cpu_cnt[req_lat_meas->tx_cpu]++;
		if (req_lat_meas->rx_cpu >= 0 && req_lat_meas->rx_cpu < NVMEIB_DFLT_MAX_CPUS)
			cpu_lat_data->rx_cpu_cnt[req_lat_meas->rx_cpu]++;
		if (req_lat_meas->rx_queue >= 0 && req_lat_meas->rx_queue < NVMEIB_DFLT_MAX_CPUS)
			cpu_lat_data->rx_queue_cnt[req_lat_meas->rx_queue]++;
		cpu_lat_data->rx_skb_hash = req_lat_meas->rx_skb_hash;
		
		local_irq_restore(flags);
	}
}

static inline int nvmeibc_nr_lat_meas_nrch_status_fill_buf(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data, char *buf, size_t len, ssize_t *count)
{
	#define BUF_ADD(...)	*count += scnprintf(buf+*count, len-*count, __VA_ARGS__)
	int rv = 0, cpu, meas_type, i;
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data *pcpu_lat_data;
	u64 *tx_cpu_cnt = kzalloc(NVMEIB_DFLT_MAX_CPUS * sizeof(u64), GFP_ATOMIC);
	u64 *rx_cpu_cnt = kzalloc(NVMEIB_DFLT_MAX_CPUS * sizeof(u64), GFP_ATOMIC);
	u64 *rx_queue_cnt = kzalloc(NVMEIB_DFLT_MAX_CPUS * sizeof(u64), GFP_ATOMIC);
	u32 rx_skb_hash = 0;

	BUF_ADD("\t\t\t\t- Latency usec num - (min/avg/max)");

	for (meas_type = 0; meas_type < C_NRCH_LAT_MAX; meas_type++) {
		u32 min_tot_usec = ~(u32)0;
		u32 max_tot_usec = 0;
		u64 tot_usec = 0;
		u64 tot_num_meas = 0;
		for_each_possible_cpu(cpu) {
			pcpu_lat_data = per_cpu_ptr(per_cpu_lat_data, cpu);
			tot_num_meas += pcpu_lat_data->lat_data[meas_type].num_meas;
			tot_usec += pcpu_lat_data->lat_data[meas_type].tot_usec;
			min_tot_usec = min(min_tot_usec, pcpu_lat_data->lat_data[meas_type].min_usec);
			max_tot_usec = max(max_tot_usec, pcpu_lat_data->lat_data[meas_type].max_usec);
		}
		if (tot_num_meas) {
			BUF_ADD("- %s: %llu - (%u/%llu/%u)", nvmeibc_nr_lat_meas_lat_type_name(meas_type),
				tot_num_meas, min_tot_usec, tot_usec / tot_num_meas, max_tot_usec);
		} else {
			BUF_ADD("- %s: 0 - (0,0,0)", nvmeibc_nr_lat_meas_lat_type_name(meas_type));
		}
	}
	BUF_ADD("\n");
	if (tx_cpu_cnt && rx_cpu_cnt && rx_queue_cnt) {
		for_each_possible_cpu(cpu) {
			pcpu_lat_data = per_cpu_ptr(per_cpu_lat_data, cpu);
			for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
				tx_cpu_cnt[i] += pcpu_lat_data->tx_cpu_cnt[i];
				rx_cpu_cnt[i] += pcpu_lat_data->rx_cpu_cnt[i];
				rx_queue_cnt[i] += pcpu_lat_data->rx_queue_cnt[i];
			}
			if (pcpu_lat_data->rx_skb_hash)
				rx_skb_hash = pcpu_lat_data->rx_skb_hash;
		}
		BUF_ADD("\t\t\t\t- TX CPU Cnt: ");
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (cpu_online(i) && tx_cpu_cnt[i])
				BUF_ADD("\t[%d]: %llu", i, tx_cpu_cnt[i]);
		}
		BUF_ADD("\n");
		BUF_ADD("\t\t\t\t- RX CPU Cnt: ");
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (cpu_online(i) && rx_cpu_cnt[i])
				BUF_ADD("\t[%d]: %llu", i, rx_cpu_cnt[i]);
		}
		BUF_ADD("\n");
		BUF_ADD("\t\t\t\t- RX Queue Cnt: ");
		for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++) {
			if (rx_queue_cnt[i])
				BUF_ADD("\t[%d]: %llu", i, rx_queue_cnt[i]);
		}
		BUF_ADD("\n");
		if (rx_skb_hash)
			BUF_ADD("\t\t\t\t- RX SKB Hash: %08x\n", rx_skb_hash);
	}
	kfree(tx_cpu_cnt);
	kfree(rx_cpu_cnt);
	kfree(rx_queue_cnt);
	
	#undef BUF_ADD	
	return rv;
}

#else
struct nvmeibc_nr_lat_meas_nrch_req_meas {};
struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data {};

static inline void nvmeibc_nr_lat_meas_clear_nrch_pcpu_data(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data)
{
	(void)per_cpu_lat_data;
}

static inline bool nvmeibc_nr_lat_meas_alloc_wc_md(bool alloc_siw_wc_md,
						struct ib_wc *wc_s, struct ib_wc *wc_r, struct ib_wc *wc_m,
						int n_wc_s, int n_wc_r, int n_wc_m, 
						void **wc_md_s, void **wc_md_r, void **wc_md_m) 
{
	(void)alloc_siw_wc_md;
	(void)wc_s;
	(void)wc_r;
	(void)wc_m;
	(void)n_wc_s;
	(void)n_wc_r;
	(void)n_wc_m;
	(void)wc_md_s;
	(void)wc_md_r;
	(void)wc_md_m;
	return true;
}

static inline void nvmeibc_nr_lat_meas_init_req(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas,
						enum nvmeibc_disk_command_type dcmd_type)
{
	(void)lat_meas;
	(void)dcmd_type;
}

static inline void nvmeibc_nr_lat_meas_sq_post(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas)
{
	(void)lat_meas;
}

static inline void nvmeibc_nr_lat_meas_send_comp(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas, 
						 const struct ib_wc *wc)
{
	(void)lat_meas;
	(void)wc;
}

static inline void nvmeibc_nr_lat_meas_recv_comp(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas,
						 const struct ib_wc *wc)
{
	(void)lat_meas;
	(void)wc;
}

static inline void nvmeibc_nr_lat_meas_recv_comp_process(struct nvmeibc_nr_lat_meas_nrch_req_meas *lat_meas)
{
	(void)lat_meas;
}

static inline void nvmeibc_nr_lat_meas_update_nrch_pcpu_data(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data,
	struct nvmeibc_nr_lat_meas_nrch_req_meas *req_lat_meas, bool got_send_comp, bool is_siw)
{
	(void)req_lat_meas;
	(void)per_cpu_lat_data;
	(void)got_send_comp;
	(void)is_siw;
}

static inline int nvmeibc_nr_lat_meas_nrch_status_fill_buf(
	struct nvmeibc_nr_lat_meas_nrch_per_cpu_lat_data __percpu *per_cpu_lat_data, char *buf, size_t len, ssize_t *count)
{
	(void)per_cpu_lat_data;
	(void)buf;
	(void)len;
	(void)count;
	return 0;
}

#endif

#endif
