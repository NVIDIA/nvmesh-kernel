#include "kr_incs.h"
#include "linux/completion.h"
#include "linux/gfp.h"
#include "linux/slab.h"
#include "nvmeibs_srv_toma_messages.h"

#ifndef HAVE_NETDEV_NET_NOTIFIER
#	define HAVE_NETDEV_NET_NOTIFIER
#endif
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>

#include "nvmeibs_disk.h"
#include "nvmeibs_types.h"
#include "nvmeib_public.h"
#include "nvmeib_shared.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_um_comm.h"
#include "nvmeibs_serjio.h"
#include "kth/nvmeib_public_kth.h"
#include "nvmeib_kth_events.h"
#include "nvmeib.h"
#include "nvmeibs_trace.h"

#include "nvmeib_utils.h"

#define THIS_PAGE_SHIFT 12
#define THIS_PAGE_SIZE (1 << THIS_PAGE_SHIFT)
#define MAX_IO_CMD_IN_PROGRESS 8

#if defined(UK_ZERO_TEST) && UK_ZERO_TEST
#	define TEST_CODE 1
#else
#	define TEST_CODE 0
#endif

#if TEST_CODE
#	define DATA_INT (0xab)
#	define MD_INT (0xcd)
#endif

#define READ_STATUS_NO_READ (-19191919)

struct zero_io_event {
	struct nvmeib_public_kth_event e;
	struct zero_req *zreq;
	struct io_req *io;
	int status;
	u32 result;
};

static inline struct zero_io_event * e_to_zio(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct zero_io_event, e);
}

static void free_zioe(struct nvmeib_public_kth_event *e)
{
}

struct io_req {
	struct page **pages;
	void *vaddr;
	void *md;
	dma_addr_t md_dma;
	unsigned long *prpl_virt;
	dma_addr_t prpl_phys;
	struct nvmeibs_nvme_req *reqs;
	struct zero_io_event *es;
	int write_in_progress;
	struct list_head link;
};

struct disk_sector_info;
struct netlink_event;
struct zero_req {
	struct disk_sector_info *dsi;
	struct io_req reqs[MAX_IO_CMD_IN_PROGRESS];
	struct list_head r;
	int prpl_n_pages;
	struct io_req zero_req;
	struct netlink_event *cur;
	unsigned long lba;
	unsigned long last_lba_plus_1;
	int read_in_progress;
	bool stopped;
	bool is_test_zero;
	unsigned long failed_sw_lba;
	bool (*need_write)(struct disk_sector_info *dsi, bool is_zero);
};

struct per_disk;
struct disk_sector_info {
	struct per_disk *pd;
	int sw_sector;
	int hw_sector;
	int n_hw_in_sw;
	int hw_md_size;
	int sw_md_size;
	int n_sw_sectors_bb;
	int bb_size_bytes;
	int dt_n_pages;
	int md_n_pages;
	struct zero_req zero;
#if TEST_CODE
	struct zero_req data;
	unsigned long n_zero_sectors;
	unsigned long n_data_sectors;
	int percentage_zero;
#endif
};

struct pd_io_event {
	struct nvmeib_public_kth_event e;
	struct per_disk *pd;
	int status;
	int result;
};

static inline struct pd_io_event * e_to_pdio(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct pd_io_event, e);
}

static void free_pdio(struct nvmeib_public_kth_event *e)
{
}

struct io_op {
	struct netlink_event *e;
	int n_sectors;
	int n_pages;
	int md_size;
	struct page **pages;
	void *vaddr;
	void *md;
	dma_addr_t md_dma;
	struct nvmeibs_nvme_req req;
	int prpl_n_pages;
	unsigned long *prpl_virt;
	dma_addr_t prpl_phys;
	struct pd_io_event io;
	bool in_progress;
	struct disk_sector_info *si;
	struct list_head link;
};

static inline struct io_op * e_to_ioop(struct pd_io_event *e)
{
	return container_of(e, struct io_op, io);
}

struct trim_op {
	struct netlink_event *e;
	struct zero_req *zreq;
	struct nvmeibs_nvme_req req;
	struct nvmeib_dsm_range *dsm_buf;
	dma_addr_t dsm_buf_dma;
	struct pd_io_event tr;
	bool in_progress;
};

struct identify_exe;
struct identify_event {
	struct nvmeib_public_kth_event e;
	struct identify_exe *ie;
};

static inline struct identify_event * e_to_ie(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct identify_event, e);
}

static void free_ie(struct nvmeib_public_kth_event *e)
{
}

struct per_disk;
struct identify_exe {
	struct nvmeib_public_kth_obj base;
	struct netlink_event *e;
	struct identify_event end;
	int data_len;
};

struct nvmeibs_um_comm;
struct pd_event;
struct per_disk {
	struct nvmeibs_um_comm *p;
	struct nvmeibs_disk_info *di;
	struct nvmeib_public_kth_obj base;
	struct pd_event *estart;
	struct pd_event *estop;
	/* disk sw sector info */
	struct disk_sector_info sw;
	/* if sw sector differs from hw sector we have the hw sector info */
	struct disk_sector_info *hw;
	bool stopped;
	struct list_head link;
	struct list_head um_events;
	bool in_progress;
	//struct io_op io_op;
	struct list_head io_ops;
	struct trim_op trim_op;
	struct nvmeib_disk_info disk_info;
	struct disk_remove_event *disk_removal_e;
	struct identify_exe ie;
};

struct dummy_disk {
	char name[256];
	struct list_head link;
};

struct nvmeibs_um_comm {
	struct nvmeib_public_kth_obj base;
	/* the start completion object */
	struct completion start;
	/* the end completion object */
	struct completion end;
	/* the netlink socket */
	struct sock *nl_sk;
	/* true when initialization finished successfully */
	bool ready;
	/* the disks */
	struct list_head disks;
	struct list_head pending_disk_stop_ack;
	struct list_head pending_disk_remove_ack;
	struct list_head formats;
	struct list_head dummy_disks;
	/* we keep the last message coming from usermode that asked for disks */
	struct list_head processes;
	bool toma_is_up;
	bool stopped;
	long next_toma_keep_alive;
};
static struct nvmeib_public_kth_id um_comm;

struct format_exe;
struct format_event {
	struct nvmeib_public_kth_event e;
	struct format_exe *fe;
};

static inline struct format_event * e_to_fe(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct format_event, e);
}

static void free_fe(struct nvmeib_public_kth_event *e)
{
}

struct format_exe {
	struct nvmeib_public_kth_obj base;
	struct nvmeibs_um_comm *p;
	struct netlink_event *e;
	char *disk_id;
	struct format_event start;
	struct format_event end;
	int rv_code;
	struct nvmeib_new_format_info info;
	struct list_head link;
};

typedef int (*repf)(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
					struct netlink_event *e, int code);

#define get_kth(p) p->base.kth
#define get_evl(p) p->base.events

static DEFINE_MUTEX(um_comm_nl_mutex);

struct pd_event {
	struct nvmeib_public_kth_event e;
	struct per_disk *pd;
};

static inline struct pd_event * e_to_pd(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct pd_event, e);
}

static void free_pde(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_pd(e));
}

struct netlink_event {
	struct nvmeib_public_kth_event e;
	int pid;
	void *p;
	ktime_t latency_ns;
	struct list_head link;
};

static inline struct netlink_event * e_to_nle(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct netlink_event, e);
}

static void free_nle(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_nle(e)->p);
	kfree(e_to_nle(e));
}

struct msg_to_process_event {
	struct nvmeib_public_kth_event e;
	struct nvmeib_msg_to_process m;
};

static inline struct msg_to_process_event * e_to_m2p(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct msg_to_process_event, e);
}

static void free_m2p(struct nvmeib_public_kth_event *e)
{
	struct msg_to_process_event *m = e_to_m2p(e);
	if (m->m.free_buf)
		m->m.free_buf(m->m.buf);
	kfree(m);
}

struct disk_add_event {
	struct nvmeib_public_kth_event e;
	struct nvmeibs_disk_info *di;
	char *name;
	int len;
};

static inline struct disk_add_event * e_to_da(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct disk_add_event, e);
}

static void free_da(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_da(e));
}

struct disk_remove_event {
	struct nvmeib_public_kth_event e;
	struct nvmeibs_disk_info *di;
	struct completion *done;
};

static inline struct disk_remove_event * e_to_dr(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct disk_remove_event, e);
}

static bool dr_no_recipient(struct nvmeib_public_kth_event *e)
{
	complete(e_to_dr(e)->done);
	return false;
}

static void free_dr(struct nvmeib_public_kth_event *e)
{
}

struct dummy_disk_add_event {
	struct nvmeib_public_kth_event e;
	char name[256];
	bool add;
};

static inline struct dummy_disk_add_event * e_to_dda(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct dummy_disk_add_event, e);
}

static void free_dda(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_dda(e));
}

struct serjio_state_change_event {
	struct nvmeib_public_kth_event e;
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u16 vendor_id;
	char model_str[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	enum nvmeibs_serjio_status serjio_status;
};

static inline struct serjio_state_change_event * e_to_srj(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct serjio_state_change_event, e);
}

static void free_srj(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_srj(e));
}

struct toma_event {
	struct nvmeib_public_kth_event e;
	bool is_up;
};

static inline struct toma_event * e_to_ta(
	struct nvmeib_public_kth_event *e)
{
	return container_of(e, struct toma_event, e);
}

static void free_ta(struct nvmeib_public_kth_event *e)
{
	kfree(e_to_ta(e));
}

struct local_client_up {
	struct nvmeib_local_client		c;
};
static struct local_client_up local_client_up_ctx;

static DEFINE_MUTEX(local_client_up_p_mutex);
static volatile struct local_client_up *local_client_up_p = NULL;

static bool zero_need_write(struct disk_sector_info *dsi, bool is_zero)
{
	return !is_zero;
}

#if TEST_CODE
static bool data_need_write(struct disk_sector_info *dsi, bool is_zero)
{
	unsigned char rand;
	bool rv;

	NFIN;
	_ND(data_need_write_d1, "is_zero=@CHAR", is_zero ? 'T' : 'F');
	if (!is_zero) {
		++dsi->n_data_sectors;
		_ND(data_need_write_d2, "dsi->n_data_sectors=@LONG", dsi->n_data_sectors);
		rv = false;
		goto out;
	}
	get_random_bytes(&rand, sizeof(rand));
	_ND(data_need_write_d3, "rand=@INT", (int)rand);
	_ND(data_need_write_d4, "dsi->percentage_zero=@INT", dsi->percentage_zero);
	if (((int)rand % 100) + 1 > dsi->percentage_zero) {
		++dsi->n_data_sectors;
		_ND(data_need_write_d5, "dsi->n_data_sectors=@LONG", dsi->n_data_sectors);
		rv = true;
		goto out;
	}
	else {
		++dsi->n_zero_sectors;
		_ND(data_need_write_d6, "dsi->n_zero_sectors=@LONG", dsi->n_zero_sectors);
		rv = false;
		goto out;
	}

out:
	NFOUT;
	return rv;
}
#endif

static int run(void *arg);
static int done(void *arg);
static void destroy(struct nvmeibs_um_comm *p);
static void um_free(struct nvmeibs_um_comm *p);
static int create_kth(struct nvmeibs_um_comm *p)
{
	char name[72];
	struct nvmeib_public_kth_params params = {
		.name = name,
		.short_name = proc_name_format("S", "UK", "um"),
		.cpu = -1, /* dont care */
		.started = false,
		.is_main = true,
		.run = run,
		.done = done,
		.run_arg = p,
		.done_arg = p
	};
	struct nvmeib_public_kth_id kth;
	int rv;

	NFIN;
	snprintf(name, sizeof(name), "S~user_mode_comm");
	if (nvmeib_public_kth_is_err(
		(kth = nvmeib_public_kth_create(&params)))) {
		_NE(error_um_comm_create_kth, "Fail to create thread for user mode communication");
		rv = -1;
	}
	else {
		wait_for_completion(&p->start);
		_ND(trace_um_comm_create_kth, "usermode kth @TOPOLOGY_INT", kth.t);
		if (p->ready)
			rv = 0;
		else {
			destroy(p);
			rv = -1;
		}
	}
	NFOUT;
	return rv;
}

struct nvmeibs_um_comm * nvmeibs_um_comm_start(void)
{
	struct nvmeibs_um_comm *p;

	NFIN;
	if (!(p = kzalloc(sizeof(*p), GFP_KERNEL))) {
		_NE(error_um_comm_nvmeibs_um_comm_start, "Failed to allocate nvmeibs_um_comm");
		goto out;
	}
	init_completion(&p->start);
	init_completion(&p->end);
	INIT_LIST_HEAD(&p->disks);
	INIT_LIST_HEAD(&p->pending_disk_stop_ack);
	INIT_LIST_HEAD(&p->pending_disk_remove_ack);
	INIT_LIST_HEAD(&p->formats);
	INIT_LIST_HEAD(&p->dummy_disks);
	INIT_LIST_HEAD(&p->processes);
	p->next_toma_keep_alive = -1;
	nvmeib_public_kth_obj_init(&p->base);
	if (create_kth(p)) {
		_NE(error_1_um_comm_nvmeibs_um_comm_start, "Failed to create nvmeibs_um_comm kth");
		goto free_p;
	}
	else
		_NT(trace_um_comm_nvmeibs_um_comm_start, "Usermode channel started...");
	goto out;

free_p:
	um_free(p);
	p = NULL;

out:
	NFOUT;
	return p;
}

void nvmeibs_um_comm_stop(struct nvmeibs_um_comm *p)
{
	NFIN;
	if (p) {
		nvmeib_public_kth_stop(get_kth(p));
		destroy(p);
		um_free(p);
	}
	_NT(trace_um_comm_nvmeibs_um_comm_stop, "Usermode channel ended...");
	NFOUT;
}

static int post_msg(struct nlmsghdr *nlh)
{
	struct netlink_event *e = NULL;
	struct nvmeib_nl_uk_comm_msg *msg;
	void *p = NULL;
	int rv;

	NFIN;
	msg = nlmsg_data(nlh);
	if (!(p = kmalloc(msg->len, GFP_KERNEL)) ||
		!(e = kzalloc(sizeof(*e), GFP_KERNEL))) {
		_NE(error_um_comm_post_msg, "Failed to allocate netlink msg");
		goto err;
	}
	memcpy(p, msg, msg->len);
	e->e.type = umc_nl;
	e->e.id = um_comm;
	e->e.free = free_nle;
	e->pid = nlh->nlmsg_pid;
	e->p = p;
	e->latency_ns = nvmeib_public_ktime_get();
	_ND(trace_um_comm_post_msg, "Posting usermode message...");
	if (nvmeib_public_kth_add_event(&e->e)) {
		_NE(error_1_um_comm_post_msg, "No recipient for detach_all message @TOPOLOGY_INT", e->e.id.t);
		goto err;
	}
	rv = 0;
	goto out;

err:
	kfree(p);
	kfree(e);
	rv = -1;

out:
	NFOUT;
	return rv;
}

static void recv_msg_(struct sk_buff *skb)
{
#if KS_NETLINK_EXTRA_ARG
	struct netlink_ext_ack extack = {};
#endif
	struct nlmsghdr *nlh;
	int msglen;
	int err;

	NFIN;
	while (skb->len >= nlmsg_total_size(0)) {
		nlh = nlmsg_hdr(skb);
		err = 0;

		_ND(recv_msg__d1, "Got a netlink message of size @INT", nlh->nlmsg_len);
		if (nlh->nlmsg_len < NLMSG_HDRLEN || skb->len < nlh->nlmsg_len) {
			_ND(recv_msg__d2, "skb->len@INT is smaller than nlh->nlmsg_len=@INT",
				skb->len, nlh->nlmsg_len);
			goto out;
		}

		/* Skip control messages */
		if (nlh->nlmsg_type < NLMSG_MIN_TYPE) {
			_ND(recv_msg__d3, "nlh->nlmsg_type=@INT is less then NLMSG_MIN_TYPE=@INT",
				nlh->nlmsg_type, NLMSG_MIN_TYPE);
			goto ack;
		}

		err = post_msg(nlh);
		if (err == -EINTR) {
			_ND(recv_msg__d4, "post_msg() failed with -EINTR");
			goto skip;
		}

ack:
		if (err) {
			_NE(recv_msg__e1, "Failed to post netlink message...");
#if KS_NETLINK_EXTRA_ARG
			netlink_ack(skb, nlh, err, &extack);
#else
			netlink_ack(skb, nlh, err);
#endif
		}

skip:
		msglen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (msglen > skb->len)
			msglen = skb->len;
		skb_pull(skb, msglen);
	}
out:
	NFOUT;
}

static void recv_msg(struct sk_buff *skb)
{
	mutex_lock(&um_comm_nl_mutex);
	if (!nvmeib_public_kth_is_err(um_comm))
		recv_msg_(skb);
	mutex_unlock(&um_comm_nl_mutex);
}

static void set_um_comm(struct nvmeibs_um_comm *p)
{
	NFIN;
	mutex_lock(&um_comm_nl_mutex);
	if (p)
		um_comm = p->base.kth;
	else
		um_comm.tns = 0;
	mutex_unlock(&um_comm_nl_mutex);
	NFOUT;
}

static bool start_nl(struct nvmeibs_um_comm *p)
{
#if KS_NETLINK_KERNEL_CREATE_CFG_PARAM
	/* this is for 3.6 kernels and above. */
	struct netlink_kernel_cfg cfg = {
		.input = recv_msg,
	};
#endif
	bool rv;

	NFIN;

#if KS_NETLINK_KERNEL_CREATE_CFG_PARAM
	p->nl_sk = netlink_kernel_create(&init_net, NETLINK_SRV_COMM, &cfg);
#else
	p->nl_sk = netlink_kernel_create(&init_net, NETLINK_SRV_COMM, 0,
									 recv_msg, NULL, THIS_MODULE);
#endif
	/**
	 *  for old kernels we need to use this one
	 *  p->nl_sk = netlink_kernel_create(&init_net, NETLINK_USER, 0,
	 *                                   recv_msg,NULL,THIS_MODULE);
	 */
	if (!p->nl_sk) {
		_NE(error_um_comm_start_nl, "Failed to creating net link socket.");
		rv = false;
	}
	else {
		set_um_comm(p);
		rv = true;
	}
	_NI(trace_um_comm_start_nl, "Usermode channel started...");
	NFOUT;
	return rv;
}

static bool filter_pd_stop(struct nvmeib_public_kth_event *e)
{
	return e->type == umc_pd_stop;
}

static bool filter_fd_stop(struct nvmeib_public_kth_event *e)
{
	return e->type == umc_format_end;
}

static void handle_toma_state(struct nvmeibs_um_comm *p, bool is_up);
static void wait_all_pd(struct nvmeibs_um_comm *p);
static void wait_all_fd(struct nvmeibs_um_comm *p);
static void handle_stop(struct nvmeibs_um_comm *p)
{
	struct per_disk *pd;

	NFIN;
	p->stopped = true;
	handle_toma_state(p, false);
	list_for_each_entry(pd, &p->disks, link)
		nvmeib_public_kth_stop(pd->base.kth);
	wait_all_fd(p);
	wait_all_pd(p);
	NFOUT;
}

static int prepare_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;

	NFIN;
	_NT(trace_um_comm_prepare_rep, "Sending reply to @UK_COMM_OPCODE_STR : pid=@PID, latency_n=@LATENCY_N, msg_id=@MSG_ID, code=@CODE",
	   uk_comm_opcode_str(rep->opcode),
		e->pid, rep->latency_ns, imsg->id, code);
	NFOUT;
	return 0;
}

static struct nvmeib_zero_disk * get_zero_disk(
	struct nvmeib_nl_uk_comm_msg *msg);
static int prepare_zero_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct zero_req *zreq = ctx;
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;
	struct nvmeib_zero_disk *zmsg;
	struct nvmeib_test_zero_reply *tzrep;

	NFIN;
	if (code && imsg->opcode == csc_test_zero_disk && zreq) {
		tzrep = container_of(rep, struct nvmeib_test_zero_reply, base);
		tzrep->failed_sw_lba = zreq->failed_sw_lba;
		zreq->failed_sw_lba = 0;
	}
	zmsg = get_zero_disk(imsg);
	_NT(trace_um_comm_prepare_zero_rep, "Sending @YES_NO_STATUS reply to @UK_COMM_OPCODE_STR@@DISK_ID_STR: pid=@PID, "
		"slba=@SLBA_LONG, n_hw_sec=@N_HW_SEC (e=@EVENT_PTR), "
		"latency_n=@LATENCY_N, msg_id=@MSG_ID",
		code == 0 ? "PASS" : "FAILED",
		uk_comm_opcode_str(rep->opcode), zreq->dsi->pd->di->disk_id,
		e->pid, zmsg->start_hw_sector, zmsg->n_hw_sectors, e, rep->latency_ns,
		imsg->id);
	NFOUT;
	return 0;
}

static int prepare_format_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct format_exe *fe = ctx;
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;
	struct nvmeib_format_disk_reply *frep;

	NFIN;
	frep = container_of(rep, struct nvmeib_format_disk_reply, base);
	memcpy(&frep->info, &fe->info, sizeof(fe->info));
	_NT(trace_um_comm_prepare_format_rep, "Sending @TRUE_FALSE_STR reply to @UK_COMM_OPCODE_STR@@DISK_ID_STR: pid=@PID, (e=@INMSG), "
		"latency_n=@LATENCY_N, msg_id=@MSG_ID",
		code == 0 ? "PASS" : "FAILED",
		uk_comm_opcode_str(rep->opcode), fe->disk_id,
		e->pid, e, rep->latency_ns, imsg->id);
	NFOUT;
	return 0;
}

static int prepare_io_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;
	struct nvmeib_io_to_disk *wd = (struct nvmeib_io_to_disk *)imsg->data;
	struct nvmeib_io_to_disk_reply *iorep;

	NFIN;
	iorep = container_of(rep, struct nvmeib_io_to_disk_reply, base);
	iorep->n_data_io = wd->data_len;
	iorep->n_md_io = wd->md_len;
	memcpy(iorep->disk_id, wd->disk_id, sizeof(iorep->disk_id));
	iorep->vendor_id = wd->vendor_id;
	_NT(trace_um_comm_prepare_io_rep, "Sending @TRUE_FALSE_STR reply to @UK_COMM_OPCODE_STR@@DISK_ID_STR: pid=@PID, (e=@INMSG), "
		"latency_n=@LATENCY_N, msg_id=@MSG_ID",
		code == 0 ? "PASS" : "FAILED",
		uk_comm_opcode_str(rep->opcode), wd->disk_id,
		e->pid, e, rep->latency_ns, imsg->id);
	NFOUT;
	return 0;
}

static int prepare_identify_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct identify_exe *ie = ctx;
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;
	struct nvmeib_identify_disk *id = (struct nvmeib_identify_disk *)imsg->data;
	struct nvmeib_identify_disk_reply *iorep;

	NFIN;
	iorep = container_of(rep, struct nvmeib_identify_disk_reply, base);
	iorep->data_len = ie->data_len;
	memcpy(iorep->disk_id, id->disk_id, sizeof(iorep->disk_id));
	_NT(prepare_identify_rep_t1, "Sending @STR reply to @STR\\@@STR: pid=@INT, (e=@PTR), "
		"latency_n=@INT64, msg_id=@LONG",
		code == 0 ? "PASS" : "FAILED",
		uk_comm_opcode_str(rep->opcode), id->disk_id,
		e->pid, e, rep->latency_ns, imsg->id);
	NFOUT;
	return 0;
}

static int post_pd_msg(struct per_disk *pd, struct netlink_event *e);
static void reply_usermode(struct nvmeibs_um_comm *p, struct netlink_event *e,
	int code, repf f, void *ctx);

static void reply_error_code(struct nvmeibs_um_comm *p, struct netlink_event *e,
	int code)
{
	reply_usermode(p, e, code, prepare_rep, NULL);
}

static void reply_error(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	reply_error_code(p, e, -1);
}

static void reply_error_zero(struct nvmeibs_um_comm *p, struct zero_req *zreq,
	struct netlink_event *e)
{
	reply_usermode(p, e, -1, prepare_zero_rep, zreq);
}

static void reply_success_zero(struct nvmeibs_um_comm *p, struct zero_req *zreq,
	struct netlink_event *e)
{
	reply_usermode(p, e, 0, prepare_zero_rep, zreq);
}

static void reply_success_format(struct nvmeibs_um_comm *p,
	struct format_exe *fe, struct netlink_event *e)
{
	reply_usermode(p, e, 0, prepare_format_rep, fe);
}

static int prepare_disks_rep(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;

	NFIN;
	_NT(trace_um_comm_prepare_disks_rep, "Sending reply to @UK_COMM_OPCODE_STR : pid=@PID, latency_n=@LATENCY_N, msg_id=@MSG_ID, code=@CODE",
	   uk_comm_opcode_str(rep->opcode),
		e->pid, rep->latency_ns, imsg->id, code);
	NFOUT;
	return 0;
}

static int prepare_copied_rsc(void *ctx, struct nvmeib_nl_uk_comm_rep *rep,
	struct netlink_event *e, int code)
{
	struct nvmeib_nl_uk_comm_msg *imsg = e->p;

	NFIN;
	_NT(trace_um_comm_prepare_copied_rsc,
		"Sending reply to @UK_COMM_OPCODE_STR : pid=@PID, latency_n=@LATENCY_N, msg_id=@MSG_ID, code=@CODE",
	   uk_comm_opcode_str(rep->opcode),
		e->pid, rep->latency_ns, imsg->id, code);
	NFOUT;
	return 0;
}

static void reply_success_io(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	reply_usermode(p, e, 0, prepare_io_rep, NULL);
}

static void reply_success_identify(
	struct nvmeibs_um_comm *p, struct netlink_event *e, struct identify_exe *ie)
{
	reply_usermode(p, e, 0, prepare_identify_rep, ie);
}

static int reply_usermode_payload(struct nvmeibs_um_comm *p,
	struct netlink_event *e, int code, void *skb,
	struct nvmeib_nl_uk_comm_msg *omsg, int msg_size,
	repf f, void *ctx);
static int send_disk_names(struct nvmeibs_um_comm *p, struct netlink_event *e,
	bool *need_reply)
{
	struct per_disk *pd;
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_get_disk_names_reply *rep;
	struct nvmeib_short_disk_info *info;
	int msg_size;
	int n = 0;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct dummy_disk *dd;
	int rv;

	NFIN;
	*need_reply = true;
	list_for_each_entry(pd, &p->disks, link)
		++n;
	list_for_each_entry(dd, &p->dummy_disks, link)
		++n;
	msg_size = sizeof(*omsg) + sizeof(*rep) + n * sizeof(*info);
	_ND(trace_um_comm_send_disk_names, "sizeof(nlmsghdr)=@SIZEOF, "
	   "sizeof(nvmeib_nl_uk_comm_msg)=@SIZEOF, "
	   "sizeof(nvmeib_get_disk_names_reply)=@SIZEOF, "
	   "disk_names_len=@STRLEN",
		sizeof(struct nlmsghdr), sizeof(*omsg), sizeof(*rep),
		n * NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	if (!(skb = nlmsg_new(msg_size, GFP_KERNEL))) {
		_NE(error_um_comm_send_disk_names, "Failed to allocate message of size @MSG_SIZE to client", msg_size);
		rv = -1;
		goto out;
	}
	nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
	omsg = nlmsg_data(nlh);
	memset(omsg, 0, msg_size);
	rep = (struct nvmeib_get_disk_names_reply *)omsg->data;
	rep->n_disks = n;
	info = rep->info;
	list_for_each_entry(pd, &p->disks, link) {
		info->is_dummy = false;
		memcpy(info->disk_id, pd->di->disk_id, sizeof(pd->di->disk_id));
		info->hw_block_size = pd->sw.hw_sector;
		info->hw_md_size = pd->sw.hw_md_size;
		info->n_blocks = pd->disk_info.n_blocks;
		info->n_hw_blocks = pd->disk_info.n_hw_blocks;
		info->sw_block_size = pd->sw.sw_sector;
		info->sw_md_size = pd->sw.sw_md_size;
		info->max_request_size = pd->disk_info.max_request_size;
		info->md_inline = pd->di->mtdt_extd ? 1 : 0;
		_NT(trace_1_um_comm_send_disk_names, "disk_name=@DISK_NAME, hw_block_size=@HW_BLOCK_SIZE, hw_md_size=@HW_MD_SIZE, n_blocks=@N_BLOCKS, n_hw_blocks=@N_HW_BLOCKS, "
		   "sw_block_size=@SW_BLOCK_SIZE, sw_md_size=@SW_MD_SIZE, max_request_size=@MAX_REQUEST_SIZE, "
		   "inline_md=@INLINE_MD",
			info->disk_id,
			info->hw_block_size, info->hw_md_size, info->n_blocks, info->n_hw_blocks,
			info->sw_block_size, info->sw_md_size,
			info->max_request_size, info->md_inline ? 'Y' : 'N');
		++info;
	}
	list_for_each_entry(dd, &p->dummy_disks, link) {
		memcpy(info->dummy_name, dd->name, sizeof(dd->name));
		++info;
	}
	*need_reply = false;
	rv = reply_usermode_payload(
		p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
	goto out;

out:
	NFOUT;
	return rv;
}

static struct netlink_event * find_process(
	struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct netlink_event *ee;
	struct nvmeib_nl_uk_comm_msg *emsg = e->p;
	struct nvmeib_nl_uk_comm_msg *eemsg;
	bool found = false;

	NFIN;
	list_for_each_entry(ee, &p->processes, link) {
		eemsg = ee->p;
		if (eemsg->caller_type == emsg->caller_type) {
			found = true;
			break;
		}
	}
	NFOUT;
	return found ? ee : NULL;
}

static void add_process(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct netlink_event *ee;
	struct nvmeib_nl_uk_comm_msg *imsg;

	NFIN;
	if ((ee = find_process(p, e))) {
		list_del(&ee->link);
		free_nle(&ee->e);
	}
	list_add_tail(&e->link, &p->processes);
	imsg = e->p;
	if (imsg->caller_type == TOMA_CALLER)
		p->toma_is_up = true;
	NFOUT;
}

static int send_disk_to_process(
	struct nvmeibs_um_comm *p, struct per_disk *pd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_disk_info_reply *rep;
	int msg_size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int rv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
		nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
		omsg = nlmsg_data(nlh);
		rep = (struct nvmeib_disk_info_reply *)omsg->data;
		rep->selector = nvmeib_disk_info_reply_dinfo;
		rep->dinfo.disk = pd->disk_info;
		rep->dinfo.serjio_status = nvmeibs_serjio_get_status(pd->di);
		if (rep->dinfo.disk.status[0] == 0) {
			strlcpy(rep->dinfo.disk.status, nvmeibs_get_status(pd->di), sizeof(rep->dinfo.disk.status));
		}
		_NT(trace_um_comm_nvmeibs_um_comm_send_disk_to_process,
			"Sending the disk info event for disk @DISK_ID_STR, state @SERJIO_STATE", pd->disk_info.disk_id, rep->dinfo.serjio_status);
		rv = reply_usermode_payload(
			p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
	}
	else {
		_NE(error_um_comm_send_disk_to_process, "Failed to allocate message of size @MSG_SIZE to client", msg_size);
		rv = -1;
	}
	NFOUT;
	return rv;
}

static int send_disk_to_processes(
	struct nvmeibs_um_comm *p, struct per_disk *pd)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct netlink_event *e;
	int rv = 0;

	NFIN;
	list_for_each_entry(e, &p->processes, link) {
		if (send_disk_to_process(p, pd, e)) {
			/* we failed sending the new disk info but we only care if TOMA
			   is registered...
			*/
			imsg = e->p;
			if (imsg->caller_type == TOMA_CALLER) {
				rv = -1;
				break;
			}
		}
	}
	NFOUT;
	return rv;
}

/**
 * Send serjio state change to a specific process
 */
static int send_serjio_state_changed_to_process(
	struct nvmeibs_um_comm *p,
	const char *disk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_status,
	struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_disk_info_reply *rep;
	int msg_size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int rv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
		nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
		omsg = nlmsg_data(nlh);
		rep = (struct nvmeib_disk_info_reply *)omsg->data;

		rep->selector = nvmeib_disk_info_reply_serjio_state;
		strlcpy(rep->serjio_state_change.disk_id, disk_id,
		        sizeof(rep->serjio_state_change.disk_id));
		rep->serjio_state_change.vendor_id = vendor_id;
		rep->serjio_state_change.serjio_status = serjio_status;
		strlcpy(rep->serjio_state_change.model_str, model_str,
		        sizeof(rep->serjio_state_change.model_str));

		_NT(trace_um_comm_nvmeibs_um_comm_send_serjio_state_changed_to_process,
			"Send the serjio_state_change event for disk @DISK_ID_STR, state @SERJIO_STATE", disk_id, serjio_status);

		rv = reply_usermode_payload(
			p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
	}
	else {
		_NE(error_um_comm_send_serjio_state_changed_to_process,
			"Failed to allocate message of size @MSG_SIZE to client", msg_size);
		rv = -1;
	}
	NFOUT;
	return rv;
}

/**
 * Send serjio state change to all awaiting response
 */
static int send_serjio_state_changed_to_processes(
	struct nvmeibs_um_comm *p,
	const char *disk_id, u16 vendor_id, char *model_str, enum nvmeibs_serjio_status serjio_status)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct netlink_event *e;
	int rv = 0;

	NFIN;
	list_for_each_entry(e, &p->processes, link) {
		if (send_serjio_state_changed_to_process(
			p, disk_id, vendor_id, model_str, serjio_status, e)) {
			/* we failed sending the new disk info but we only care if TOMA
			   is registered...
			*/
			imsg = e->p;
			if (imsg->caller_type == TOMA_CALLER) {
				rv = -1;
				break;
			}
		}
	}
	NFOUT;
	return rv;
}

/**
 * KTH event handler - serjio notifies on state change
 */
static void handle_serjio_state_changed(struct nvmeibs_um_comm *p, struct serjio_state_change_event *e) {
	NFIN;
	send_serjio_state_changed_to_processes(p, e->disk_id, e->vendor_id, e->model_str, e->serjio_status);
	NFOUT;
}

/**
 * Public function, initiate serjio state update via the kth worker
 */
void nvmeibs_um_comm_serjio_state_changed(
    struct nvmeibs_um_comm *p, const char *disk_id, u16 vendor_id,
	char *model_str, enum nvmeibs_serjio_status serjio_status) {
	DECLARE_COMPLETION_ONSTACK(done);
	struct serjio_state_change_event *e;

	NFIN;
	_NT(trace_um_comm_nvmeibs_um_comm_serjio_state_changed,
	    "Send the serjio_state_change event for disk @DISK_ID_STR, state @SERJIO_STATE", disk_id, serjio_status);

	/* @TODO: Can it be GFP_KERNEL? In theory it can be interrup ctx though... */
	if (!(e = kzalloc(sizeof(*e), GFP_ATOMIC)))
		goto out;

	e->e.type = umc_serjio_state;
	e->e.id = um_comm;
	e->e.free = free_srj;

	strlcpy(e->disk_id, disk_id, sizeof(e->disk_id));
	e->vendor_id = vendor_id;
	memcpy(e->model_str, model_str, sizeof(e->model_str));
	e->serjio_status = serjio_status;

	if (nvmeib_public_kth_add_event(&e->e)) {
		_NE(error_um_comm_nvmeibs_um_comm_serjio_state_changed,
		    "No recipient for @DISK_ID_STR serjio_state_change message", disk_id);
		kfree(e);
	}

out:
	NFOUT;
}

static int prepare_send_to_process(void *ctx,
	struct nvmeib_nl_uk_comm_rep *rep, struct netlink_event *e, int code)
{
	struct nvmeib_nl_uk_comm_msg *omsg = ctx;

	NFIN;
	omsg->opcode = csc_msg_to_process;
	rep->opcode = csc_msg_to_process;
	NFOUT;
	return 0;
}

static void handle_send_msg_to_process(
	struct nvmeibs_um_comm *p, struct msg_to_process_event *e)
{
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_push_extended_msg *rep;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct netlink_event *n;
	int rv = -1;

	NFIN;
	list_for_each_entry(n, &p->processes, link) {
		if (n->pid == e->m.pid) {
			const int msg_size = sizeof(*omsg) + sizeof(*rep) + e->m.buf_len;
			if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
				nlh = nlmsg_put(skb, n->pid, 0, NLMSG_DONE, msg_size, 0);
				omsg = nlmsg_data(nlh);
				rep = &((struct nvmeib_nl_msg_to_toma*)omsg->data)->payload.extended_msg;
				rep->n_bytes_len = e->m.buf_len;
				memcpy(rep->content, e->m.buf, e->m.buf_len);
				_NT(hsmtp_t1, "Send msg to process_id @INT", n->pid);
				rv = reply_usermode_payload(p, n, 0, skb, omsg, msg_size,
						prepare_send_to_process, omsg);
			}
			else
				_NE(hsmtp_e1, "Failed to allocate message of size "
					"@MSG_SIZE to client", msg_size);
			break;
		}
	}
	if (e->m.on_send_comp)
		e->m.on_send_comp(e->m.ctx, rv);
	NFOUT;
}

static int send_dummy_disk_to_process(
	struct nvmeibs_um_comm *p, struct dummy_disk *dd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_disk_info_reply *rep;
	int msg_size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int rv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
		nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
		omsg = nlmsg_data(nlh);
		rep = (struct nvmeib_disk_info_reply *)omsg->data;
		rep->selector = nvmeib_disk_info_reply_dummy;
		rep->remove_disk = false;
		memcpy(rep->dummy_name, dd->name, sizeof(rep->dummy_name));
		_NT(trace_um_comm_send_dummy_disk_to_process, "Sending dummy disk @STR to userspace", dd->name);
		rv = reply_usermode_payload(
			p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
	}
	else {
		_NE(error_um_comm_send_dummy_disk_to_process, "Failed to allocate message of size @MSG_SIZE to client", msg_size);
		rv = -1;
	}
	NFOUT;
	return rv;
}

static int send_dummy_disk_to_processes(
	struct nvmeibs_um_comm *p, struct dummy_disk *dd)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct netlink_event *e;
	int rv = 0;

	NFIN;
	list_for_each_entry(e, &p->processes, link) {
		if (send_dummy_disk_to_process(p, dd, e)) {
			/* we failed sending the new disk info but we only care if TOMA
			   is registered...
			*/
			imsg = e->p;
			if (imsg->caller_type == TOMA_CALLER) {
				rv = -1;
				break;
			}
		}
	}
	NFOUT;
	return rv;
}

static int send_remove_disk(struct nvmeibs_um_comm *p, struct per_disk *pd)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_disk_info_reply *rep;
	int msg_size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct netlink_event *e;
	int rv = -1;
	int rvv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	list_for_each_entry(e, &p->processes, link) {
		if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
			nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
			omsg = nlmsg_data(nlh);
			rep = (struct nvmeib_disk_info_reply *)omsg->data;
			rep->selector = nvmeib_disk_info_reply_dinfo;
			rep->dinfo.disk = pd->disk_info;
			rep->dinfo.disk.n_blocks = 0;
			rvv = reply_usermode_payload(
				p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
		}
		else {
			_NE(error_um_comm_send_remove_disk, "Failed to allocate message of size @MSG_SIZE to client",
				msg_size);
			rvv = -1;
		}
		imsg = e->p;
		/* rv = 0 means we need an ack from the registrant and the only
		   registarnt we care about is toma
		*/
		if (imsg->caller_type == TOMA_CALLER && rvv == 0)
			rv = 0;
	}
	NFOUT;
	return rv;
}

static int send_remove_dummy_disk(
	struct nvmeibs_um_comm *p, struct dummy_disk *dd)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_disk_info_reply *rep;
	int msg_size;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct netlink_event *e;
	int rv = -1;
	int rvv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	list_for_each_entry(e, &p->processes, link) {
		if ((skb = nlmsg_new(msg_size, GFP_KERNEL))) {
			nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
			omsg = nlmsg_data(nlh);
			rep = (struct nvmeib_disk_info_reply *)omsg->data;
			rep->selector = nvmeib_disk_info_reply_dummy;
			rep->remove_disk = true;
			memcpy(rep->dummy_name, dd->name, sizeof(rep->dummy_name));
			rvv = reply_usermode_payload(
				p, e, 0, skb, omsg, msg_size, prepare_disks_rep, NULL);
		}
		else {
			_NE(error_um_comm_send_remove_dummy_disk, "Failed to allocate message of size @MSG_SIZE to client",
				msg_size);
			rvv = -1;
		}
		imsg = e->p;
		/* rv = 0 means we need an ack from the registrant and the only
		   registarnt we care about is toma
		*/
		if (imsg->caller_type == TOMA_CALLER && rvv == 0)
			rv = 0;
	}
	NFOUT;
	return rv;
}

static void send_disks(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct per_disk *pd;
	struct dummy_disk *dd;

	NFIN;
	add_process(p, e);
	list_for_each_entry(pd, &p->disks, link)
		send_disk_to_process(p, pd, e);
	list_for_each_entry(dd, &p->dummy_disks, link)
		send_dummy_disk_to_process(p, dd, e);
	NFOUT;
}

static void finish_remove_disk(
	struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_remove_disk *rd;
	struct per_disk *pd;
	bool found = false;

	NFIN;
	msg = e->p;
	rd = (struct nvmeib_remove_disk *)msg->data;
	_NT(trace_um_comm_finish_remove_disk, "Usermode acked removal of disk @DISK_ID_STR (ack_id=@ACK_ID_LONG)",
		rd->disk_id, rd->ack_id);
	list_for_each_entry(pd, &p->pending_disk_remove_ack, link)
		if (!memcmp(pd->di->disk_id, rd->disk_id, sizeof(pd->di->disk_id))) {
			found = true;
			break;
		}
	if (found) {
		_NT(trace_1_um_comm_finish_remove_disk, "Wakeup the waiting disk @DISK_ID_STR removal thread...", pd->di->disk_id);
		complete(pd->disk_removal_e->done);
		list_del(&pd->link);
		kfree(pd);
	}
	NFOUT;
}

static struct nvmeib_zero_disk * get_zero_disk(
	struct nvmeib_nl_uk_comm_msg *msg)
{
	struct nvmeib_zero_disk *zd = NULL;

	NFIN;
	if (msg->opcode == csc_zero_disk ||
		msg->opcode == csc_test_zero_disk)
		zd = (struct nvmeib_zero_disk *)msg->data;
#if TEST_CODE
	else if (msg->opcode == csc_contaminate_disk)
		zd = &((struct nvmeib_contaminate_disk *)msg->data)->base;
#endif
	NFOUT;
	return zd;
}

static struct per_disk * find_pd(struct nvmeibs_um_comm *p,
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE])
{
	struct per_disk *pd = NULL;
	bool found = false;

	NFIN;
	list_for_each_entry(pd, &p->disks, link)
		if (!memcmp(disk_id, pd->di->disk_id, sizeof(pd->di->disk_id))) {
			_ND(trace_um_comm_find_pd, "Found disk @DISK_ID_STR", disk_id);
			found = true;
			break;
		}
	NFOUT;
	return found ? pd : NULL;
}

static void send_ready_fd(struct format_exe *fe)
{
	NFIN;
	nvmeib_public_kth_add_event(&fe->start.e);
	NFOUT;
}

static int fd_run(void *arg)
{
	struct format_exe *fe = arg;
	struct nvmeib_nl_uk_comm_msg *msg = fe->e->p;
	struct nvmeib_format_disk *fd = (struct nvmeib_format_disk *)msg->data;

	NFIN;
	_NT(trace_um_comm_fd_run, "Started format thread for disk @DISK_ID_STR...", fe->disk_id);
	nvmeib_public_kth_switch_current_eq(&fe->base.events);
	send_ready_fd(fe);
	nvmeib_public_kth_wait_resume();
	fe->rv_code = nvmeibs_nvme_format_disk(fe->disk_id, fd, &fe->info) ?
		csce_format_failed : csce_ok;
	_NT(trace_1_um_comm_fd_run, "Format thread for disk @DISK_ID_STR is done (rv=@INT)", fe->disk_id, fe->rv_code);
	NFOUT;
	return -1;
}

static int fd_done(void *arg)
{
	struct format_exe *fe = arg;

	NFIN;
	nvmeib_public_kth_add_event(&fe->end.e);
	NFOUT;
	return 0;
}

static int launch_format(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct format_exe *fe;
	char name[72];
	struct nvmeib_public_kth_params params = {
		.name = name,
		.short_name = proc_name_format("S", "UK", "fmum"),
		.cpu = -1, /* dont care */
		.started = false,
		.is_main = true,
		.run = fd_run,
		.done = fd_done,
	};
	struct nvmeib_public_kth_id kth;
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_format_disk *fd;
	int rv;

	NFIN;
	msg = e->p;
	fd = (struct nvmeib_format_disk *)msg->data;
	if (!(fe = kzalloc(sizeof(*fe), GFP_KERNEL))) {
		_NE(error_um_comm_launch_format, "Failed to allocate format executor");
		rv = csce_format_oom;
		goto out;
	}
	else {
		INIT_LIST_HEAD(&fe->link);
		fe->p = p;
		fe->e = e;
		fe->disk_id = fd->disk_id;
		fe->start.e.type = umc_format_start;
		fe->start.e.id = um_comm;
		fe->start.e.free = free_fe;
		fe->start.fe = fe;
		fe->end.e.type = umc_format_end;
		fe->end.e.id = um_comm;
		fe->end.e.free = free_fe;
		fe->end.fe = fe;
		params.run_arg = fe;
		params.done_arg = fe;
	}
	nvmeib_public_kth_obj_init(&fe->base);
	snprintf(name, sizeof(name), "S~%s", fd->disk_id);
	if (nvmeib_public_kth_is_err(
		(kth = nvmeib_public_kth_create(&params)))) {
		_NE(error_1_um_comm_launch_format, "Fail to create format user-mode comm @DISK_ID_STR", fd->disk_id);
		rv = csce_format_thread;
	}
	else {
		fe->base.kth = kth;
		list_add_tail(&fe->link, &p->formats);
		rv = csce_ok;
	}

out:
	NFOUT;
	return rv;
}

static int handle_format(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_format_disk *fd;
	struct format_exe *fe;
	bool found = false;
	int rv;

	NFIN;
	msg = e->p;
	fd = (struct nvmeib_format_disk *)msg->data;
	_ND(trace_um_comm_handle_format, "Format disk from @DISK_ID_STR, vendor_id=@VENDOR_ID, id=@ID",
		fd->disk_id, fd->vendor_id, msg->id);
	list_for_each_entry(fe, &p->formats, link)
		if (!memcmp(fe->disk_id, fd->disk_id, sizeof(fd->disk_id))) {
			found = true;
			break;
		}
	if (found) {
		_NE(error_um_comm_handle_format, "Format for disk @DISK_ID_STR is already in progress", fe->disk_id);
		rv = csce_format_in_progress;
	}
	else
		rv = launch_format(p, e);
	NFOUT;
	return rv;
}

static inline unsigned long wait_for_toma_dt(void)
{
	return msecs_to_jiffies(2 * TOMA_SILENCE_MAX_PERIOD_SECS * 1000);
}

static inline bool is_keep_alive(struct nvmeib_public_kth_event *e)
{
	return e->type == umc_nl &&
		(((struct nvmeib_nl_uk_comm_msg *)(e_to_nle(e)->p))->opcode ==
		 csc_keep_alive);
}

static int send_copied_rscs(
	struct nvmeibs_um_comm *p, struct netlink_event *e, pid_t pid, void *rsc)
{
	struct nvmeib_nl_uk_comm_msg *omsg;
	struct nvmeib_copied_rscs_reply *rep;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int msg_size;
	int rv;

	NFIN;
	msg_size = sizeof(*omsg) + sizeof(*rep);
	_ND(scr_1, "sizeof(nlmsghdr)=@SIZEOF, "
	   "sizeof(nvmeib_nl_uk_comm_msg)=@SIZEOF, "
	   "sizeof(nvmeib_copied_rscs_reply)=@SIZEOF",
		sizeof(struct nlmsghdr), sizeof(*omsg), sizeof(*rep));
	if (!(skb = nlmsg_new(msg_size, GFP_KERNEL))) {
		_NE(scr_2, "Failed to allocate message of size @MSG_SIZE to client",
			msg_size);
		rv = -1;
		goto out;
	}
	nlh = nlmsg_put(skb, pid, 0, NLMSG_DONE, msg_size, 0);
	omsg = nlmsg_data(nlh);
	memset(omsg, 0, msg_size);
	rep = (struct nvmeib_copied_rscs_reply *)omsg->data;
	rep->rsc = rsc;
	rv = reply_usermode_payload(
		p, e, 0, skb, omsg, msg_size, prepare_copied_rsc, NULL);

out:
	NFOUT;
	return rv;
}

static bool handle_toma_client_msg(struct nvmeibs_um_comm *p,
	struct netlink_event *e, struct nvmeib_msg_tom_2_local_clnt *msg)
{
	struct nvmeib_toma_client *m;
	struct nvmeib_local_client_params lc_params = {};
	void *vbuffer;
	void *buffer = NULL;
	struct page **pages;
	int i, rv = -1;
	int	n_pages_pinned;
	const pid_t toma_pid = e->pid;	// Filled in post_msg(), no need for explicit passing of msg->toma_pid

	NFIN;
	m = &msg->toma_client;
	if (m->copy) {
		if ((unsigned long)m->data % PAGE_SIZE) {
			_NE(htcm_1, "copy from user space must be page align m->data=@PTR", m->data);
			goto done;
		}
		if (!(m->data && m->n_pages)) {
			_NE(htcm_4, "copy from useer space must data of non-zero size");
			goto done;
		}
		if (!(pages = kzalloc(sizeof(*pages) * m->n_pages, GFP_KERNEL))) {
			_NE(htcm_2, "failed to allocated pages array");
			goto done;
		}
		n_pages_pinned = nvmeib_public_user_pages_for_io_pin(toma_pid, (unsigned long)m->data, m->n_pages, pages, false);
		if (n_pages_pinned != m->n_pages) {
			_NE(htcm_3, "failed to pin user pages n_pages_pinned=@INT", n_pages_pinned);
			goto free_pages;
		}
		if (!(buffer = kmalloc(m->n_pages * PAGE_SIZE, GFP_KERNEL))) {
			_NE(htcm_5, "failed to allocate local buffer");
			goto unpin;
		}
		for (i = 0; i < m->n_pages; ++i) {
			if ((vbuffer = kmap_atomic(pages[i]))) {
				memcpy(buffer + i * PAGE_SIZE, vbuffer, PAGE_SIZE);
				kunmap_atomic(vbuffer);
			}
			else {
				kfree(buffer);
				buffer = NULL;
				goto unpin;
			}
		}
		rv = 0;
	}
	else {
		m->data = NULL;
		buffer = NULL;
		rv = 0;
		goto done;
	}

unpin:
	nvmeib_public_user_pages_for_io_unpin(m->n_pages, pages, false);

free_pages:
	kfree(pages);

done:
	if (!rv) {
		if (buffer) {
			lc_params.copy = true;
			lc_params.msg = buffer;
			lc_params.msg_len = m->n_pages * PAGE_SIZE;
		}
		else {
			lc_params.copy = false;
			lc_params.msg = m->data;
		}
		mutex_lock(&local_client_up_p_mutex);
		if (!local_client_up_p) {
			_NE(ycbwkwo, "local_client_up_p=NULL");
		} else {
			_NT(4bs7jqi, "local_cl m->data=@PTR msg_len=@INT msg=@PTR copy=@INT", m->data, lc_params.msg_len, lc_params.msg, lc_params.copy);
			local_client_up_p->c.toma_request_f(&lc_params);
		}
		mutex_unlock(&local_client_up_p_mutex);
		rv = send_copied_rscs(p, e, toma_pid, m->data);
	}
	NFOUT;
	return rv;
}

static bool handle_nl(struct nvmeibs_um_comm *p, struct netlink_event *e)
{
	struct per_disk *pd;
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_zero_disk *zd;
	struct nvmeib_io_to_disk *wd;
	struct nvmeib_identify_disk *id;
	bool delete_msg = false;
	bool failed = false;
	bool need_reply = false;
	char *disk_id;
	int code = -1;

	NFIN;
	msg = e->p;
	switch ((enum nvmeibs_um_caller_type)msg->caller_type) {
	case TOMA_CALLER:
		p->toma_is_up = true;
		p->next_toma_keep_alive = jiffies + wait_for_toma_dt();
		if (!find_process(p, e))
			_NT(trace_um_comm_handle_nl, "TOMA is up...");
		break;
	case INFRA_CALLER:
		if (!find_process(p, e))
			_NT(trace_1_um_comm_handle_nl, "Welcome Infra...");
		break;
	case LOCAL_CLNT_CALLER:
		_NE(obqj3jk, "Unexpected LOCAL_CLNT_CALLER");
		break;
	default:
		_NT(trace_2_um_comm_handle_nl, "Caller is @CHAR", msg->caller_type);
		break;
	}
	if (!is_keep_alive(&e->e))
		_NT(trace_3_um_comm_handle_nl, "handling message @UK_COMM_OPCODE_STR", uk_comm_opcode_str(msg->opcode));
	else
		_ND(trace_4_um_comm_handle_nl, "handling message @UK_COMM_OPCODE_STR", uk_comm_opcode_str(msg->opcode));
	if (msg->opcode <= csc_start || msg->opcode >= csc_end)
		goto error;
	switch (msg->opcode) {
	case csc_zero_disk:
	case csc_test_zero_disk:
#if TEST_CODE
	case csc_contaminate_disk:
#endif
		if (!(zd = get_zero_disk(msg))) {
			_NE(error_um_comm_handle_nl, "Failed to get va pointer to the zero disk base struct");
			goto error;
		}
		else {
			_ND(trace_5_um_comm_handle_nl, "Zero disk range msg from @DISK_ID_STR, vendor_id=@VENDOR_ID, id=@ID - "
			   "range @START_HW_SECTOR-@N_HW_SECTORS", zd->disk_id, zd->vendor_id, msg->id,
				zd->start_hw_sector,
				zd->start_hw_sector + zd->n_hw_sectors - 1);
			disk_id = zd->disk_id;
		}
		break;
	case csc_format_disk:
		if ((code = handle_format(p, e)) == csce_ok)
			goto out;
		else
			goto error;
	case csc_io_to_disk:
		wd = (struct nvmeib_io_to_disk *)msg->data;
		_ND(trace_6_um_comm_handle_nl, "Io to disk from @DISK_ID_STR, vendor_id=@VENDOR_ID, id=@ID",
			wd->disk_id, wd->vendor_id, msg->id);
		disk_id = wd->disk_id;
		break;
	case csc_identify_disk:
		id = (struct nvmeib_identify_disk *)msg->data;
		_ND(handle_nl_d1, "Identify disk from @STR", id->disk_id);
		disk_id = id->disk_id;
		break;
	case csc_get_disk_names:
		failed = !!send_disk_names(p, e, &need_reply);
		delete_msg = true;
		goto out;
	case csc_get_disks:
		send_disks(p, e);
		need_reply = false;
		delete_msg = false;
		goto out;
	case csc_remove_disk_ack:
		finish_remove_disk(p, e);
		need_reply = false;
		delete_msg = true;
		goto out;
	case csc_keep_alive:
		failed = false;
		need_reply = false;
		delete_msg = true;
		goto out;
	case csc_local_client:
		if (!local_client_up_p) {
			_NE(handle_nl_e1001, "No local client");
			goto error;
		}
		if ((failed = handle_toma_client_msg(p, e,
			(struct nvmeib_msg_tom_2_local_clnt *)msg->data))) {
			need_reply = true;
		}
		else {
			need_reply = false;
		}
		delete_msg = true;
		goto out;
	default:
		disk_id = NULL;
		break;
	}
	if (disk_id && (pd = find_pd(p, disk_id))) {
		if (post_pd_msg(pd, e))
			goto error;
	}
	else {
error:
		delete_msg = true;
		failed = true;
		need_reply = true;
	}

out:
	if (failed && need_reply)
		code == -1 ? reply_error(p, e) : reply_error_code(p, e, code);
	NFOUT;
	return delete_msg;
}

static struct per_disk * find_disk(struct nvmeibs_um_comm *p,
	struct nvmeibs_disk_info *di, struct list_head *disks)
{
	struct per_disk *pd;
	bool found = false;

	NFIN;
	list_for_each_entry(pd, disks, link)
		if (pd->di == di) {
			found = true;
			break;
		}
	NFOUT;
	return found ? pd : NULL;
}

static int start_pd(struct per_disk *pd);
static void handle_disk_add(struct nvmeibs_um_comm *p, struct disk_add_event *e)
{
	struct per_disk *pd = NULL;
	struct pd_event *estart = NULL;
	struct pd_event *estop = NULL;

	NFIN;
	if ((pd = find_disk(p, e->di, &p->disks))) {
		_NT(trace_um_comm_handle_disk_add, "Adding disk @DISK_ID_STR that is already registered", e->di->disk_id);
		goto out;
	}
	else
		_NI(trace_1_um_comm_handle_disk_add, "Adding new disk @DISK_ID_STR", e->di->disk_id);
	if (!(pd = kzalloc(sizeof(*pd), GFP_KERNEL)) ||
		!(estart = kzalloc(sizeof(*e), GFP_KERNEL)) ||
		!(estop = kzalloc(sizeof(*e), GFP_KERNEL))) {
		_NT(trace_2_um_comm_handle_disk_add, "Fail to allocate memory for a new disk @DISK_ID_STR", e->di->disk_id);
		kfree(pd);
		kfree(estart);
		kfree(estop);
		goto out;
	}
	estart->e.type = umc_pd_start;
	estart->e.id = um_comm;
	estart->e.free = free_pde;
	estart->pd = pd;
	pd->estart = estart;
	estop->e.type = umc_pd_stop;
	estop->e.id = um_comm;
	estop->e.free = free_pde;
	pd->estop = estop;
	estop->pd = pd;
	pd->p = p;
	pd->di = e->di;
	nvmeibs_nvme_fill_disk_info(pd->di, &pd->disk_info);
	INIT_LIST_HEAD(&pd->um_events);
	INIT_LIST_HEAD(&pd->io_ops);
	INIT_LIST_HEAD(&pd->link);
	if (start_pd(pd)) {
		_NE(error_um_comm_handle_disk_add, "Fail to start new pd for disk @DISK_ID_STR", e->di->disk_id);
		goto error_pd;
	}
	list_add_tail(&pd->link, &p->disks);
	goto out;

error_pd:
	kfree(e);
	kfree(pd);

out:
	NFOUT;
}

static void handle_disk_remove(
	struct nvmeibs_um_comm *p, struct disk_remove_event *e)
{
	struct per_disk *pd;

	NFIN;
	if ((pd = find_disk(p, e->di, &p->disks))) {
		_NI(trace_um_comm_handle_disk_remove, "Removing disk @DISK_ID_STR", e->di->disk_id);
		pd->disk_removal_e = e;
		list_del(&pd->link);
		list_add_tail(&pd->link, &p->pending_disk_stop_ack);
		nvmeib_public_kth_stop(pd->base.kth);
	}
	else if ((pd = find_disk(p, e->di, &p->pending_disk_stop_ack)) ||
			 (pd = find_disk(p, e->di, &p->pending_disk_remove_ack))) {
		_NI(trace_1_um_comm_handle_disk_remove, "Removing disk @DISK_ID_STR that is in the process of being removed",
			e->di->disk_id);
		pd->disk_removal_e = e;
	}
	else {
		_NT(trace_2_um_comm_handle_disk_remove, "trying to remove non-registered disk @DISK_ID_STR", e->di->disk_id);
		complete(e->done);
	}
	NFOUT;
}

static void handle_dummy_disk_add(struct nvmeibs_um_comm *p,
	struct dummy_disk_add_event *e)
{
	struct dummy_disk *dd;

	NFIN;
	if ((dd = kzalloc(sizeof(*dd), GFP_KERNEL))) {
		memcpy(dd->name, e->name, sizeof(dd->name));
		list_add_tail(&dd->link, &p->dummy_disks);
		send_dummy_disk_to_processes(p, dd);
	}
	NFOUT;
}

static void handle_dummy_disk_remove(struct nvmeibs_um_comm *p,
	struct dummy_disk_add_event *e)
{
	struct dummy_disk *dd;
	bool found = false;

	NFIN;
	list_for_each_entry(dd, &p->dummy_disks, link) {
		if (memcmp(dd->name, e->name, sizeof(e->name)) == 0) {
			list_del(&dd->link);
			found = true;
			break;
		}
	}
	if (found) {
		send_remove_dummy_disk(p, dd);
		kfree(dd);
	}
	NFOUT;
}

static void handle_pd_start(struct nvmeibs_um_comm *p, struct pd_event *e)
{
	struct per_disk *pd;
	bool found = false;
	int rv;

	NFIN;
	list_for_each_entry(pd, &p->disks, link)
	if (pd == e->pd) {
		found = true;
		break;
	}
	if (found) {
		if ((rv = nvmeib_public_kth_resume(get_kth(pd))))
			_NE(error_um_comm_handle_pd_start, "Failed to resume per_disk kth of disk @DISK_ID_STR - @RV",
			   pd->di->disk_id, rv);
	}
	else
		_NE(error_1_um_comm_handle_pd_start, "Got a pd_start_event from pd=@PD on non_registered pd", e->pd);
	NFOUT;
}

static void handle_pd_stop(struct nvmeibs_um_comm *p, struct per_disk *pd)
{
	NFIN;
	_NT(trace_um_comm_handle_pd_stop, "per_disk @DISK_ID_STR self triggered stop", pd->di->disk_id);
	nvmeib_public_kth_free(get_kth(pd));
	if (!list_empty(&pd->link))
		list_del(&pd->link);
	nvmeib_public_kth_obj_free(&pd->base);
	if (!p->stopped) {
		if (!send_remove_disk(p, pd) && p->toma_is_up)
			list_add_tail(&pd->link, &p->pending_disk_remove_ack);
		else {
			if (pd->disk_removal_e)
				complete(pd->disk_removal_e->done);
			kfree(pd);
		}
	}
	else
		kfree(pd);
	NFOUT;
}

static void handle_fd_start(struct nvmeibs_um_comm *p, struct format_event *e)
{
	struct format_exe *fe;
	bool found = false;
	int rv;

	NFIN;
	list_for_each_entry(fe, &p->formats, link)
	if (fe == e->fe) {
		found = true;
		break;
	}
	if (found) {
		if ((rv = nvmeib_public_kth_resume(get_kth(fe))))
			_NE(error_um_comm_handle_fd_start, "Failed to resume format_disk kth of disk @DISK_ID_STR - @RV",
				fe->disk_id, rv);
	}
	else
		_NE(error_1_um_comm_handle_fd_start, "Got a format_start_event from fe=@FE on non_registered fe", e->fe);
	NFOUT;
}

static void handle_fd_stop(struct nvmeibs_um_comm *p, struct format_exe *fe)
{
	NFIN;
	_NT(trace_um_comm_handle_fd_stop, "format_disk @DISK_ID_STR self triggered stop", fe->disk_id);
	nvmeib_public_kth_free(get_kth(fe));
	if (!list_empty(&fe->link))
		list_del(&fe->link);
	nvmeib_public_kth_obj_free(&fe->base);
	fe->rv_code ?
		reply_error_code(p, fe->e, fe->rv_code) :
		reply_success_format(p, fe, fe->e);
	free_nle(&fe->e->e);
	kfree(fe);
	NFOUT;
}

static void complete_pending_disk_removals(struct nvmeibs_um_comm *p)
{
	struct per_disk *pd;

	NFIN;
	if (!p->toma_is_up) {
		while ((pd = list_first_entry_or_null(
			&p->pending_disk_remove_ack, struct per_disk, link))) {
			_NT(complete_pending_disk_removals_t1, "Wakeup the waiting disk @STR removal thread...",
				pd->di->disk_id);
			if (pd->disk_removal_e->done)
				complete(pd->disk_removal_e->done);
			list_del(&pd->link);
			kfree(pd);
		}
	}
	NFOUT;
}

static void handle_toma_state(struct nvmeibs_um_comm *p, bool is_up)
{
	NFIN;
	p->toma_is_up = is_up;
	if (!p->toma_is_up)
		p->next_toma_keep_alive = -1;
	complete_pending_disk_removals(p);
	NFOUT;
}

static long get_next_timeout(struct nvmeibs_um_comm *p)
{
	unsigned long now;
	long rv;

	NFIN;
	if (p->next_toma_keep_alive < 0 || !p->toma_is_up)
		rv = -1;
	else {
		now = jiffies;
		rv = (p->next_toma_keep_alive > now) ?
			(p->next_toma_keep_alive - now) : 0;
	}
	_ND(trace_um_comm_get_next_timeout, "Main eventloop next timeout in msecs @JIFFIES_TO_MSECS", jiffies_to_msecs(rv));
	NFOUT;
	return rv;
}

static void _remove_local_client(void)
{
	_NT(remove_local_client, "Removing local client from server");
	mutex_lock(&local_client_up_p_mutex);
	local_client_up_p = NULL;
	mutex_unlock(&local_client_up_p_mutex);
}

static void wait_events(struct nvmeibs_um_comm *p)
{
	struct nvmeib_public_kth_event *e;
	bool cont = true;
	bool delete_msg;

	NFIN;
	_NT(trace_um_comm_wait_events, "Starting user mode communication event loop");
	while (cont && !nvmeib_public_kth_obj_wait_events(
		&p->base, get_next_timeout(p), NULL)) {
		while (cont && (e = list_first_entry_or_null(
			&p->base.events, struct nvmeib_public_kth_event, link))) {
			list_del(&e->link);
			delete_msg = true;
			if (!is_keep_alive(e))
				_NT(wait_events_t1, "Handling event @STR (@INT)",
					nvmeib_kth_event_to_str((int)e->type), (int)e->type);
			else
				_ND(wait_events_d1, "Handling event @STR (@INT)",
					nvmeib_kth_event_to_str((int)e->type), (int)e->type);
			switch (e->type) {
			case nke_stop:
				handle_stop(p);
				cont = false;
				break;
			case umc_nl:
				delete_msg = handle_nl(p, e_to_nle(e));
				break;
			case umc_send_msg_process:
				handle_send_msg_to_process(p, e_to_m2p(e));
				break;
			case umc_disk_add:
				handle_disk_add(p, e_to_da(e));
				break;
			case umc_disk_remove:
				/* this one is an external event and is allocated on the stack
				   of a different thread so once the fuction is out the
				   event is invalid...
				*/
				handle_disk_remove(p, e_to_dr(e));
				complete_pending_disk_removals(p);
				delete_msg = false;
				break;
			case umc_dummy_disk_add:
				handle_dummy_disk_add(p, e_to_dda(e));
				break;
			case umc_dummy_disk_remove:
				handle_dummy_disk_remove(p, e_to_dda(e));
				break;
			case umc_pd_start:
				handle_pd_start(p, e_to_pd(e));
				break;
			case umc_pd_stop:
				handle_pd_stop(p, e_to_pd(e)->pd);
				break;
			case umc_format_start:
				handle_fd_start(p, e_to_fe(e));
				delete_msg = false;
				break;
			case umc_format_end:
				handle_fd_stop(p, e_to_fe(e)->fe);
				delete_msg = false;
				break;
			case umc_toma_state:
				handle_toma_state(p, e_to_ta(e)->is_up);
				break;
			case umc_serjio_state:
				handle_serjio_state_changed(p, e_to_srj(e));
				break;
			case umc_local_client_down:
				_remove_local_client();
				break;
			case nke_timeout:
				handle_toma_state(p, false);
				break;
			default:
				break;
			}
			if (delete_msg)
				nvmeib_public_kth_event_free(e);
		}
	}
	NFOUT;
}

static void stop_nl(struct nvmeibs_um_comm *p)
{
	NFIN;
	_ND(trace_um_comm_stop_nl, "Stopping the netlink socket...");
	if (p->nl_sk)
		netlink_kernel_release(p->nl_sk);
	set_um_comm(NULL);
	p->nl_sk = NULL;
	NFOUT;
}

static void set_local_client(struct nvmeib_local_client *c, void *ctx)
{
	NFIN;
	(void)ctx;
	if (c) {
		mutex_lock(&local_client_up_p_mutex);
		_NT(racgquk, "toma_request_f=@PTR local_client_up_p=@PTR", c->toma_request_f, (void *)local_client_up_p);
		local_client_up_ctx.c = *c;
		local_client_up_p = &local_client_up_ctx;
		mutex_unlock(&local_client_up_p_mutex);
	}
	NFOUT;
}

static void remove_local_client(void *ctx)
{
	DECLARE_COMPLETION_ONSTACK(done);

	NFIN;
	_remove_local_client();
	NFOUT;
}

static int run(void *arg)
{
	struct nvmeibs_um_comm *p = arg;

	NFIN;
	_NT(trace_um_comm_run, "Started main usermode channel thread...");
	p->base.kth = nvmeib_public_kth_current();
	nvmeib_public_kth_switch_current_eq(&p->base.events);
	p->ready = start_nl(p);
	nvmeib_set_local_client_notification_calbacks(
		set_local_client, p, remove_local_client);
	complete(&p->start);
	if (p->ready) {
		wait_events(p);
		stop_nl(p);
	}
	nvmeib_set_local_client_notification_calbacks(
		NULL, p, NULL);
	NFOUT;
	return -1;
}

static int done(void *arg)
{
	struct nvmeibs_um_comm *p = arg;

	NFIN;
	_NT(trace_um_comm_done, "Main usermode channel is gone...");
	complete(&p->end);
	NFOUT;
	return 0;
}

static void destroy(struct nvmeibs_um_comm *p)
{
	struct netlink_event *e;
	struct dummy_disk *dd;

	NFIN;
	wait_for_completion(&p->end);
	nvmeib_public_kth_free(get_kth(p));
	while ((dd = list_first_entry_or_null(&p->dummy_disks,
		struct dummy_disk, link))) {
		list_del(&dd->link);
		kfree(dd);
	}
	while ((e = list_first_entry_or_null(
		&p->processes, struct netlink_event, link))) {
		list_del(&e->link);
		free_nle(&e->e);
	}
	p->ready = false;
	NFOUT;
}

static void um_free(struct nvmeibs_um_comm *p)
{
	NFIN;
	if (p) {
		nvmeib_public_kth_obj_free(&p->base);
		kfree(p);
	}
	NFOUT;
}

int nvmeibs_send_msg_to_user_porcess(struct nvmeib_msg_to_process *msg)
{
	struct msg_to_process_event *e;
	int rv;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		e->e.type = umc_send_msg_process;
		e->e.id = um_comm;
		e->e.free = free_m2p;
		e->m = *msg;
		if (nvmeib_public_kth_add_event(&e->e)) {
			_NE(ssmtup_e1,
				"No recipient for send_msg_to_process @TOPOLOGY_INT", e->e.id.t);
			kfree(e);
			rv = -1;
		}
		else
			rv = 0;
	}
	else {
		_NE(ssmtup_e2, "Failed to allocate send_msg_to_process message");
		rv = -1;
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeibs_send_msg_to_user_porcess);

void nvmeibs_um_comm_add_disk(
	struct nvmeibs_um_comm *p, struct nvmeibs_disk_info *di)
{
	struct disk_add_event *e;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		_ND(trace_um_comm_nvmeibs_um_comm_add_disk, "Send the add_disk event...");
		e->e.type = umc_disk_add;
		e->e.id = um_comm;
		e->e.free = free_da;
		e->di = di;
		if (nvmeib_public_kth_add_event(&e->e)) {
			_NE(error_um_comm_nvmeibs_um_comm_add_disk, "No recipient for add_disk message @TOPOLOGY_INT", e->e.id.t);
			kfree(e);
		}
	}
	else
		_NE(error_1_um_comm_nvmeibs_um_comm_add_disk, "Failed to allocate disk_add message");
	NFOUT;
}

void nvmeibs_um_comm_remove_disk(
	struct nvmeibs_um_comm *p, struct nvmeibs_disk_info *di)
{
	DECLARE_COMPLETION_ONSTACK(done);
	struct disk_remove_event e = {};

	NFIN;
	_NT(trace_um_comm_nvmeibs_um_comm_remove_disk, "Send the remove_disk event for disk @DISK_ID_STR...", di->disk_id);
	e.e.type = umc_disk_remove;
	e.e.id = um_comm;
	e.e.free = free_dr;
	e.e.no_recipient = dr_no_recipient;
	e.di = di;
	e.done = &done;
	if (!nvmeib_public_kth_add_event(&e.e)) {
		wait_for_completion(&done);
		_NT(trace_1_um_comm_nvmeibs_um_comm_remove_disk, "The remove_disk event handling for disk @DISK_ID_STR finished...",
			di->disk_id);
	}
	else
		_NE(error_um_comm_nvmeibs_um_comm_remove_disk, "No recipient for @DISK_ID_STR disk_remove message @TOPOLOGY_INT",
			di->disk_id, e.e.id.t);
	NFOUT;
}

void nvmeibs_um_comm_add_dummy_disk(
	struct nvmeibs_um_comm *p, const char *name, int len)
{
	struct dummy_disk_add_event *e;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		_ND(trace_um_comm_nvmeibs_um_comm_dummy_add_disk, "Send the add_dummy_disk event...");
		e->e.type = umc_dummy_disk_add;
		e->e.id = um_comm;
		e->e.free = free_dda;
		e->add = true;
		memcpy(e->name, name, min(((int)sizeof(e->name)), len));
		if (nvmeib_public_kth_add_event(&e->e)) {
			_NE(error_um_comm_nvmeibs_um_comm_dummy_add_disk, "No recipient for add_dummy_disk message @TOPOLOGY_INT", e->e.id.t);
			kfree(e);
		}
	}
	else
		_NE(error_1_um_comm_nvmeibs_um_comm_dummy_add_disk, "Failed to allocate dummy_disk_add message");
	NFOUT;
}

void nvmeibs_um_comm_remove_dummy_disk(
	struct nvmeibs_um_comm *p, const char *name, int len)
{
	struct dummy_disk_add_event *e;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		_ND(trace_um_comm_nvmeibs_um_comm_dummy_remove_disk, "Send the remove_dummy_disk event...");
		e->e.type = umc_dummy_disk_remove;
		e->e.id = um_comm;
		e->e.free = free_dda;
		e->add = false;
		memcpy(e->name, name, min(((int)sizeof(e->name)), len));
		if (nvmeib_public_kth_add_event(&e->e)) {
			_NE(error_um_comm_nvmeibs_um_comm_dummy_remove_disk, "No recipient for remove_dummy_disk message @TOPOLOGY_INT", e->e.id.t);
			kfree(e);
		}
	}
	else
		_NE(error_1_um_comm_nvmeibs_um_comm_dummy_remove_disk, "Failed to allocate dummy_disk_remove message");
	NFOUT;
}

void nvmeibs_um_comm_update_toma_state(struct nvmeibs_um_comm *p, bool is_up)
{
	struct toma_event *e;

	NFIN;
	if ((e = kzalloc(sizeof(*e), GFP_ATOMIC))) {
		_ND(trace_um_comm_nvmeibs_um_comm_update_toma_state, "Send the toma @TRUE_FALSE_STR event...", is_up ? "up" : "down");
		e->e.type = umc_toma_state;
		e->e.id = um_comm;
		e->e.free = free_ta;
		e->is_up = is_up;
		if (nvmeib_public_kth_add_event(&e->e)) {
			_NE(error_um_comm_nvmeibs_um_comm_update_toma_state, "No recipient for toma_@TRUE_FALSE_STR message @TOPOLOGY_INT",
				is_up ? "up" : "down", e->e.id.t);
			kfree(e);
		}
	}
	else
		_NE(error_1_um_comm_nvmeibs_um_comm_update_toma_state, "Failed to allocate toma_@TRUE_FALSE_STR message", is_up ? "up" : "down");
	NFOUT;
}

static int pd_run(void *arg);
static int pd_done(void *arg);
static int start_pd(struct per_disk *pd)
{
	char name[72];
	struct nvmeib_public_kth_params params = {
		.name = name,
		.short_name = proc_name_format("S", "UK", "pdum"),
		.cpu = -1, /* dont care */
		.started = false,
		.is_main = true,
		.run = pd_run,
		.done = pd_done,
		.run_arg = pd,
		.done_arg = pd
	};
	struct nvmeib_public_kth_id kth;
	int rv __attribute__((unused));

	NFIN;
	nvmeib_public_kth_obj_init(&pd->base);
	snprintf(name, sizeof(name), "S~%s", pd->di->disk_id);
	if (nvmeib_public_kth_is_err(
		(kth = nvmeib_public_kth_create(&params)))) {
		_NE(error_um_comm_start_pd, "Fail to create per_disk user-mode comm @DISK_ID_STR", pd->di->disk_id);
		rv = -1;
	}
	else
		pd->base.kth = kth;
	NFOUT;
	return 0;
}

static int wait_pd_stop(struct per_disk **pd)
{
	struct nvmeib_public_kth_event *e;
	int rv;

	NFIN;
	if (!(rv = nvmeib_public_kth_wait_events(filter_pd_stop, false, &e))) {
		_ND(trace_um_comm_wait_pd_stop, "pd @DISK_ID_STR finished", e_to_pd(e)->pd->di->disk_id);
		*pd = e_to_pd(e)->pd;
		nvmeib_public_kth_event_free(e);
		rv = 0;
	}
	else {
		_NE(error_um_comm_wait_pd_stop, "Fail to wait for pd to stop");
		*pd = NULL;
		rv = -1;
	}
	NFOUT;
	return rv;
}

static void wait_all_pd(struct nvmeibs_um_comm *p)
{
	struct per_disk *pd;

	NFIN;
	while (!list_empty(&p->disks) && !wait_pd_stop(&pd))
		if (pd)
			handle_pd_stop(p, pd);
	NFOUT;
}

static int wait_fd_stop(struct format_exe **fe)
{
	struct nvmeib_public_kth_event *e;
	int rv;

	NFIN;
	if (!(rv = nvmeib_public_kth_wait_events(filter_fd_stop, false, &e))) {
		_ND(trace_um_comm_wait_fd_stop, "format_disk @DISK_ID_STR finished", e_to_fe(e)->fe->disk_id);
		*fe = e_to_fe(e)->fe;
		nvmeib_public_kth_event_free(e);
		rv = 0;
	}
	else {
		_NE(error_um_comm_wait_fd_stop, "Fail to wait for pd to stop");
		*fe = NULL;
		rv = -1;
	}
	NFOUT;
	return rv;
}

static void wait_all_fd(struct nvmeibs_um_comm *p)
{
	struct format_exe *fe;

	NFIN;
	while (!list_empty(&p->formats) && !wait_fd_stop(&fe))
		if (fe)
			handle_fd_stop(p, fe);
	NFOUT;
}

static void send_ready_pd(struct per_disk *pd)
{
	NFIN;
	nvmeib_public_kth_add_event(&pd->estart->e);
	NFOUT;
}

static void wait_pd_events(struct per_disk *pd);
static int perpare_pd(struct per_disk *pd);
static void free_pd(struct per_disk *pd);
static int pd_run(void *arg)
{
	struct per_disk *pd = arg;

	NFIN;
	_NT(trace_um_comm_pd_run, "Started uesrmode channel thread for disk @DISK_ID_STR...", pd->di->disk_id);
	nvmeib_public_kth_switch_current_eq(&pd->base.events);
	send_ready_pd(pd);
	nvmeib_public_kth_wait_resume();
	if (!perpare_pd(pd))
		wait_pd_events(pd);
	free_pd(pd);
	_NT(trace_1_um_comm_pd_run, "End of uesrmode channel thread for disk @DISK_ID_STR...", pd->di->disk_id);
	NFOUT;
	return -1;
}

static int pd_done(void *arg)
{
	struct per_disk *pd = arg;

	NFIN;
	nvmeib_public_kth_add_event(&pd->estop->e);
	NFOUT;
	return 0;
}

static int post_pd_msg(struct per_disk *pd, struct netlink_event *e)
{
	int rv;

	NFIN;
	e->e.id = pd->base.kth;
	rv = nvmeib_public_kth_add_event(&e->e);
	NFOUT;
	return rv;
}

static inline int nvmeibs_max_nl_reply(void)
{
	return sizeof(struct nvmeib_nl_uk_comm_msg) + sizeof(struct nvmeib_nl_msg_to_toma);
}

static void reply_usermode(struct nvmeibs_um_comm *p, struct netlink_event *e,
	int code, repf f, void *ctx)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	const int msg_size = nvmeibs_max_nl_reply();
	struct nvmeib_nl_uk_comm_msg *omsg;

	NFIN;
	_ND(trace_um_comm_reply_usermode, "Send reply to usermode request, code=@CODE...", code);
	skb = nlmsg_new(msg_size, GFP_KERNEL);
	if (skb) {
		nlh = nlmsg_put(skb, e->pid, 0, NLMSG_DONE, msg_size, 0);
		omsg = nlmsg_data(nlh);
		reply_usermode_payload(p, e, code, skb, omsg, msg_size, f, ctx);
	}
	else
		_NE(error_um_comm_reply_usermode, "Failed to allocate new skb to reply the client");
	NFOUT;
}

static struct nvmeib_nl_uk_comm_rep * get_rep(
	struct nvmeib_nl_uk_comm_msg *msg)
{
	struct nvmeib_nl_uk_comm_rep *rep = NULL;

	NFIN;
	_ND(trace_um_comm_get_rep, "msg->opcode=@OPCODE_STR", uk_comm_opcode_str(msg->opcode));
	switch (msg->opcode) {
	case csc_get_disk_names:
		rep = &((struct nvmeib_get_disk_names_reply *)msg->data)->base;
		break;
	case csc_zero_disk:
		rep = &((struct nvmeib_zero_disk_reply *)msg->data)->base;
		break;
	case csc_test_zero_disk:
		rep = &((struct nvmeib_test_zero_reply *)msg->data)->base;
		break;
	case csc_get_disks:
		rep = &((struct nvmeib_disk_info_reply *)msg->data)->base;
		break;
	case csc_format_disk:
		rep = &((struct nvmeib_format_disk_reply *)msg->data)->base;
		break;
	case csc_io_to_disk:
		rep = &((struct nvmeib_io_to_disk_reply *)msg->data)->base;
		break;
	case csc_identify_disk:
		rep = &((struct nvmeib_identify_disk_reply *)msg->data)->base;
		break;
	case csc_local_client:
		rep = &((struct nvmeib_copied_rscs_reply *)msg->data)->base;
		break;
#if TEST_CODE
	case csc_contaminate_disk:
		rep = &((struct nvmeib_contaminate_disk_reply *)msg->data)->base;
		break;
#endif
	case csc_remove_disk_ack: default:	// All those codes do not require response from the server
		break;
	};
	NFOUT;
	return rep;
}

static int reply_usermode_payload(struct nvmeibs_um_comm *p,
	struct netlink_event *e, int code, void *skb,
	struct nvmeib_nl_uk_comm_msg *omsg, int msg_size,
	repf f, void *ctx)
{
	struct nvmeib_nl_uk_comm_msg *imsg;
	struct nvmeib_nl_uk_comm_rep *rep;
	int pid;
	int rv;
	ktime_t t = nvmeib_public_ktime_get();

	NFIN;
	_ND(trace_um_comm_reply_usermode_payload, "Send reply to usermode request...");
	pid = e->pid;
	imsg = e->p;
	_ND(trace_1_um_comm_reply_usermode_payload, "omsg=@OMSG, imsg=@IMSG, imsg_opcode=@IMSG_OPCODE, msg_size=@MSG_SIZE",
		omsg, imsg, uk_comm_opcode_str(imsg->opcode), msg_size);
	omsg->opcode = imsg->opcode;
	omsg->id = imsg->id;
	omsg->caller_type = imsg->caller_type;
	omsg->len = msg_size;
	/*
	 * When we push message to process the initial opcode omsg is wrong as it
	 * is taken from the message that registered the toma process.
	 * The reply in the following line is taken by the opcode and we need the
	 * reply type of the send_msg_to_process.  This works because
	 * all replies starts with the the same base type namely:
	 * `struct nvmeib_nl_uk_comm_rep`
	 */
	rep = get_rep(omsg);
	if (!rep) {
		BUG();
	}
	rep->opcode = omsg->opcode;
	rep->error = code == 0 ? csce_ok :
			(code == -1 ? -((int)csce_failed) : -((int)code));
	rep->latency_ns = ktime_to_ns(ktime_sub(t, e->latency_ns));
	f(ctx, rep, e, code);
	rv = netlink_unicast(p->nl_sk, skb, pid, MSG_DONTWAIT);
	/**
	 * the blocking version
	 * rv = netlink_unicast(nls, skb, pid, 0);
	 */
	if (rv < 0)
		_NE(error_um_comm_reply_usermode_payload, "fail to send error reply for message @ID (rv=@RV)", imsg->id, rv);
	else
		rv = 0;
	NFOUT;
	return rv;
}

/* Deprecated. IOMMU */
#if 0
static inline void *dma_alloc_coherent_opt(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	void *p;

	if (dev != NULL)
		return dma_alloc_coherent(dev, size, dma_handle, flag);

	if ((p = alloc_pages_exact(size, flag)) == NULL)
		return NULL;

	*dma_handle = (dma_addr_t)virt_to_phys(p);
	return p;
}

static inline void dma_free_coherent_opt(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle)
{
	if (dev == NULL)
		free_pages_exact(cpu_addr, size);
	else
		dma_free_coherent(dev, size, cpu_addr, dma_handle);
}
#endif

static void prepare_common_inline(
	struct per_disk *pd, struct disk_sector_info *dsi)
{
	int block_size;
	int bytes;

	NFIN;
	block_size = dsi->sw_sector + dsi->sw_md_size;
	bytes = pd->di->max_request_size * dsi->hw_sector;
	dsi->n_sw_sectors_bb = bytes / block_size;
	dsi->bb_size_bytes = dsi->n_sw_sectors_bb * block_size;
	dsi->dt_n_pages = DIV_ROUND_UP(dsi->bb_size_bytes, PAGE_SIZE);
	dsi->md_n_pages = 0;
	NFOUT;
}

static void prepare_common_sep(
	struct per_disk *pd, struct disk_sector_info *dsi)
{
	int n_sw_md_page;
	int max_sw_sectors_req;
	int bytes;

	NFIN;
	/* the max sw_sector in a bounce buffer */
	n_sw_md_page = THIS_PAGE_SIZE / dsi->sw_md_size;
	bytes = pd->di->max_request_size * dsi->hw_sector;
	max_sw_sectors_req = bytes / dsi->sw_sector;
	dsi->n_sw_sectors_bb = min(n_sw_md_page, max_sw_sectors_req);
	dsi->bb_size_bytes = dsi->n_sw_sectors_bb * dsi->sw_sector;
	dsi->dt_n_pages = DIV_ROUND_UP(dsi->bb_size_bytes, PAGE_SIZE);
	dsi->md_n_pages = DIV_ROUND_UP(
		dsi->n_sw_sectors_bb * dsi->sw_md_size, PAGE_SIZE);
	NFOUT;
}

static void prepare_common_no_md(
	struct per_disk *pd, struct disk_sector_info *dsi)
{
	int bytes;

	NFIN;
	/* the max sw_sector in a bounce buffer */
	bytes = pd->di->max_request_size * dsi->hw_sector;
	dsi->n_sw_sectors_bb = bytes / dsi->sw_sector;
	dsi->bb_size_bytes = dsi->n_sw_sectors_bb * dsi->sw_sector;
	dsi->dt_n_pages = DIV_ROUND_UP(dsi->bb_size_bytes, PAGE_SIZE);
	dsi->md_n_pages = 0;
	NFOUT;
}

static void prepare_common_params(struct per_disk *pd, int sector_shift,
	struct disk_sector_info *dsi)
{
	NFIN;
	dsi->pd = pd;
	dsi->sw_sector = 1 << sector_shift;
	dsi->hw_sector = pd->di->block_size;
	dsi->n_hw_in_sw = 1 << (sector_shift - pd->di->block_shift);
	if (pd->di->metadata) {
		dsi->hw_md_size = pd->di->metadata;
		dsi->sw_md_size = dsi->hw_md_size * dsi->n_hw_in_sw;
	}
	else {
		dsi->hw_md_size = 0;
		dsi->sw_md_size = 0;
	}
	NFOUT;
}

static inline bool has_hw_zero(struct per_disk *pd)
{
	return pd->hw && (pd->hw != &pd->sw);
}

static void post_io(void *arg, int status, u32 result);
static void prepare_io_op(struct per_disk *pd, struct io_op *w)
{
	NFIN;
	w->io.e.type = umc_pd_io;
	w->io.e.id = pd->base.kth;
	w->io.e.free = free_pdio;
	w->io.pd = pd;
	w->req.cb = post_io;
	w->req.arg = &w->io;
	NFOUT;
}

static void prepare_ie_op(struct per_disk *pd)
{
	NFIN;
	pd->ie.end.e.type = umc_identify_end;
	pd->ie.end.e.id = pd->base.kth;
	pd->ie.end.e.free = free_ie;
	NFOUT;
}

static void post_trim(void *arg, int status, u32 result)
{
	struct pd_io_event *e = arg;

	NFIN;
	e->status = status;
	e->result = result;
	_ND(trace_um_comm_post_trim, "Done pd trim for pd of disk @DISK_ID_STR", e->pd->di->disk_id);
	if (nvmeib_public_kth_add_event(&e->e))
		_NE(error_um_comm_post_trim, "No recipient for trim message @TOPOLOGY_INT", e->e.id.t);
	NFOUT;
}

static void prepare_trim_op(struct per_disk *pd)
{
	NFIN;
	pd->trim_op.tr.e.type = umc_pd_trim;
	pd->trim_op.tr.e.id = pd->base.kth;
	pd->trim_op.tr.e.free = free_pdio;
	pd->trim_op.tr.pd = pd;
	pd->trim_op.req.cb = post_trim;
	pd->trim_op.req.arg = &pd->trim_op.tr;
	NFOUT;
}

static int prepare_common(struct per_disk *pd)
{
	int rv;

	NFIN;
	if (NVMEIBC_SECTOR_SHIFT < pd->di->block_shift) {
		_NE(error_um_comm_prepare_common, "FATAL: SW sector shift @SECTOR_SHIFT is less than HW sector shift @BLOCK_SHIFT_INT",
			NVMEIBC_SECTOR_SHIFT, pd->di->block_shift);
		rv = -1;
		goto out;
	}
	//prepare_io_op(pd);
	prepare_ie_op(pd);
	prepare_trim_op(pd);
	prepare_common_params(pd, NVMEIBC_SECTOR_SHIFT, &pd->sw);
	if (NVMEIBC_SECTOR_SHIFT != pd->di->block_shift) {
		if (!(pd->hw = kzalloc(sizeof(*pd->hw), GFP_KERNEL))) {
			_NE(error_1_um_comm_prepare_common, "FATA: failed to allocate HW sector info object");
			rv = -1;
			goto out;
		}
		else
			prepare_common_params(pd, pd->di->block_shift, pd->hw);
	}
	else
		pd->hw = &pd->sw;

	if (pd->di->mtdt_extd)
		prepare_common_inline(pd, &pd->sw);
	else if (pd->di->metadata)
		prepare_common_sep(pd, &pd->sw);
	else
		prepare_common_no_md(pd, &pd->sw);
	_NT(trace_um_comm_prepare_common, "disk @DISK_ID_STR, SW: max_request_size_hw_sectors=@MAX_REQUEST_SIZE_HW_SECTORS, sw_sector=@SW_SECTOR, "
	   "hw_sector=@HW_SECTOR, n_hw_in_sw=@N_HW_IN_SW, hw_md_size=@HW_MD_SIZE, "
	   "sw_md_size=@SW_MD_SIZE, n_sw_md_in_page=@N_SW_MD_IN_PAGE, max_sw_sectors_in_req=@MAX_SW_SECTORS_IN_REQ, "
	   "n_sw_sectors_in_bb=@N_SW_SECTORS_IN_BB, bb_size_bytes=@BB_SIZE_BYTES, data_n_pages=@DATA_N_PAGES, "
	   "md_n_pages=@MD_N_PAGES",
		pd->di->disk_id, pd->di->max_request_size,
		pd->sw.sw_sector, pd->sw.hw_sector, pd->sw.n_hw_in_sw,
		pd->sw.hw_md_size, pd->sw.sw_md_size, pd->sw.md_n_pages,
	   	pd->sw.n_sw_sectors_bb, pd->sw.n_sw_sectors_bb, pd->sw.bb_size_bytes,
	    pd->sw.dt_n_pages, pd->sw.md_n_pages);
	pd->sw.zero.dsi = &pd->sw;
	pd->sw.zero.need_write = zero_need_write;
#if TEST_CODE
	pd->sw.data.dsi = &pd->sw;
	pd->sw.data.need_write = data_need_write;
#endif
	if (has_hw_zero(pd)) {
		if (pd->di->mtdt_extd)
			prepare_common_inline(pd, pd->hw);
		else if (pd->di->metadata)
			prepare_common_sep(pd, pd->hw);
		else
			prepare_common_no_md(pd, pd->hw);
		_NT(trace_1_um_comm_prepare_common, "disk @DISK_ID_STR, SW: max_request_size_hw_sectors=@MAX_REQUEST_SIZE_HW_SECTORS, sw_sector=@SW_SECTOR, "
		   "hw_sector=@HW_SECTOR, n_hw_in_sw=@N_HW_IN_SW, hw_md_size=@HW_MD_SIZE, "
		   "sw_md_size=@SW_MD_SIZE, n_sw_md_in_page=@N_SW_MD_IN_PAGE, max_sw_sectors_in_req=@MAX_SW_SECTORS_IN_REQ, "
		   "n_sw_sectors_in_bb=@N_SW_SECTORS_IN_BB, bb_size_bytes=@BB_SIZE_BYTES, data_n_pages=@DATA_N_PAGES, "
		   "md_n_pages=@MD_N_PAGES",
		   pd->di->disk_id, pd->di->max_request_size,
		   pd->hw->sw_sector, pd->hw->hw_sector, pd->hw->n_hw_in_sw,
		   pd->hw->hw_md_size, pd->hw->sw_md_size, pd->hw->md_n_pages,
		   pd->hw->n_sw_sectors_bb, pd->hw->n_sw_sectors_bb,
		   pd->hw->bb_size_bytes, pd->hw->dt_n_pages, pd->hw->md_n_pages);
		pd->hw->zero.dsi = pd->hw;
		pd->hw->zero.need_write = zero_need_write;
#if TEST_CODE
		pd->hw->data.dsi = pd->hw;
		pd->hw->data.need_write = data_need_write;
#endif
	}
	pd->disk_info.max_n_hw_sectors = pd->hw->n_sw_sectors_bb;
	pd->disk_info.max_n_sw_sectors = pd->sw.n_sw_sectors_bb;
	rv = 0;

out:
	NFOUT;
	return rv;
}

static void free_req(
	struct per_disk *pd, struct zero_req *zreq, struct io_req *req)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_pages = dsi->dt_n_pages + dsi->md_n_pages;
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	int i;

	NFIN;
	if (req->reqs) {
		kfree(req->reqs);
		req->reqs = NULL;
	}
	if (req->es) {
		kfree(req->es);
		req->es = NULL;
	}
	if (req->vaddr) {
		nvmeib_public_vunmap(req->vaddr);
		req->vaddr = 0;
	}
	if (req->pages) {
		for (i = 0; i < n_pages; ++i) {
			if (!pd->di->external) {
				BUG_ON(!dev);
				if (req->prpl_virt[i] && !dma_mapping_error(dev, req->prpl_virt[i])) {
					dma_unmap_page(dev, req->prpl_virt[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
				}
			}
			if (req->pages[i]) {
				__free_pages(req->pages[i], 0);
				req->pages[i] = 0;
			}
		}
	}
	kfree(req->pages);
	req->pages = 0;
	if (req->prpl_virt) {
		if (!pd->di->external) {
			BUG_ON(!dev);
			dma_free_coherent(dev, zreq->prpl_n_pages * PAGE_SIZE,
					  req->prpl_virt, req->prpl_phys);
			req->prpl_phys = 0;
		} else {
			free_pages_exact(req->prpl_virt, zreq->prpl_n_pages * PAGE_SIZE);
		}
		req->prpl_virt = 0;
	}
	req->md = 0;
	req->md_dma = 0;
	NFOUT;
}

static int prepare_req(
	struct per_disk *pd, struct zero_req *zreq, struct io_req *req)
{
	struct disk_sector_info *dsi = zreq->dsi;
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	int n_pages = dsi->dt_n_pages + dsi->md_n_pages;
	int i;
	int rv = -1;

	NFIN;
	INIT_LIST_HEAD(&req->link);
	if (!(req->reqs = kzalloc(
		dsi->n_sw_sectors_bb * sizeof(*req->reqs), GFP_KERNEL))) {
		_NE(error_um_comm_prepare_req, "Failed to allocate nvmeibs_nvme_req reqs for pd @DISK_ID_STR",
			pd->di->disk_id);
		goto error;
	}
	if (!(req->es = kzalloc(
		dsi->n_sw_sectors_bb * sizeof(*req->es), GFP_KERNEL))) {
		_NE(error_1_um_comm_prepare_req, "Failed to allocate zero_io_event events for pd @DISK_ID_STR",
			pd->di->disk_id);
		goto error;
	}
	if (!pd->di->external) {
		BUG_ON(!dev);
		req->prpl_virt = dma_alloc_coherent(
			dev, zreq->prpl_n_pages * PAGE_SIZE, &req->prpl_phys, GFP_KERNEL | __GFP_ZERO);
	} else {
		req->prpl_virt = (void *)alloc_pages_exact(zreq->prpl_n_pages * PAGE_SIZE, GFP_KERNEL | __GFP_ZERO);
	}
	if (!req->prpl_virt) {
		_NE(error_2_um_comm_prepare_req, "Failed to allocate prpl for pd @DISK_ID_STR", pd->di->disk_id);
		goto error;
	}
	if (!(req->pages = kcalloc(n_pages, sizeof(*req->pages), GFP_KERNEL))) {
		_NE(error_3_um_comm_prepare_req, "Failed to allocate page tables for pd @DISK_ID_STR", pd->di->disk_id);
		goto error;
	}
	for (i = 0; i < n_pages; ++i) {
		req->pages[i] = alloc_pages_node(dev ? dev->numa_node : NUMA_NO_NODE, GFP_KERNEL, 0);
		if (!req->pages[i]) {
			_NE(error_4_um_comm_prepare_req, "Failed to allocate page @PAGE_NUM for pd @DISK_ID_STR", i, pd->di->disk_id);
			goto error;
		}
		if (!pd->di->external) {
			BUG_ON(!dev);
			req->prpl_virt[i] = dma_map_page(dev, req->pages[i], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, req->prpl_virt[i])) {
				_NE(error_6_um_comm_prepare_req, "Failed to map page @PAGE_NUM for pd @DISK_ID_STR", i, pd->di->disk_id);
				goto error;
			}
		} else {
			/* External device, nvme layer will translate virtual addresses to bio pages */
			req->prpl_virt[i] = (unsigned long)page_address(req->pages[i]);
		}
	}
	/* Sync prpl with device */
	if (!pd->di->external)
		dma_sync_single_for_device(dev, req->prpl_phys, zreq->prpl_n_pages * PAGE_SIZE, DMA_TO_DEVICE);
	if (!(req->vaddr = vmap(req->pages, n_pages, VM_MAP, PAGE_KERNEL))) {
		_NE(error_5_um_comm_prepare_req, "Fail to vmap virt area for pd @DISK_ID_STR", pd->di->disk_id);
		goto error;
	}
	for (i = 0; i < dsi->n_sw_sectors_bb; ++i) {
		INIT_LIST_HEAD(&req->reqs[i].link);
		req->reqs[i].use_sg = false;
		req->es[i].e.id = pd->base.kth;
		req->es[i].e.free = free_zioe;
		req->es[i].io = req;
		req->es[i].zreq = zreq;
	}
	req->md = page_address(req->pages[dsi->dt_n_pages]);
	req->md_dma = req->prpl_virt[dsi->dt_n_pages];
	rv = 0;
	goto out;

error:
	free_req(pd, zreq, req);

out:
	NFOUT;
	return rv;
}

static void free_reqs(struct per_disk *pd)
{
	int i;

	NFIN;
	for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i)
		free_req(pd, &pd->sw.zero, &pd->sw.zero.reqs[i]);
	if (has_hw_zero(pd))
		for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i)
			free_req(pd, &pd->hw->zero, &pd->hw->zero.reqs[i]);
#if TEST_CODE
	for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i)
		free_req(pd, &pd->sw.data, &pd->sw.data.reqs[i]);
	if (has_hw_zero(pd))
		for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i)
			free_req(pd, &pd->hw->data, &pd->hw->data.reqs[i]);
#endif
	NFOUT;
}

static void free_zero(struct per_disk *pd, struct zero_req *req)
{
	NFIN;
	free_req(pd, req, &req->zero_req);
	NFOUT;
}

static int prepare_zero(struct per_disk *pd, struct zero_req *zreq,
	int data_int, int md_int, const char *str)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_pages = dsi->dt_n_pages + dsi->md_n_pages;
	int i;
	int rv;
	void *p;

	NFIN;
	if (!(rv = prepare_req(pd, zreq, &zreq->zero_req))) {
		memset(zreq->zero_req.vaddr, data_int, n_pages * PAGE_SIZE);
		if (pd->di->mtdt_extd) {
			_NT(trace_um_comm_prepare_zero, "Disk @DISK_ID_STR - preparing @STR req for inline MD",
				pd->di->disk_id, str);
			p = zreq->zero_req.vaddr;
			for (i = 0; i < dsi->n_sw_sectors_bb; ++i) {
				p += dsi->sw_sector;
				memset(p, md_int, dsi->sw_md_size);
				p += dsi->sw_md_size;
			}
		}
		else {
			if (pd->di->metadata)
				_NT(trace_1_um_comm_prepare_zero, "Disk @DISK_ID_STR - preparing @STR req for separate MD",
					pd->di->disk_id, str);
			else
				_NT(trace_2_um_comm_prepare_zero, "Disk @DISK_ID_STR - preparing @STR req for no MD",
					pd->di->disk_id, str);
			p = page_address(zreq->zero_req.pages[dsi->dt_n_pages]);
			memset(p, md_int, dsi->md_n_pages * PAGE_SIZE);
		}
		rv = 0;
	}
	else {
		free_zero(pd, zreq);
		rv = -1;
	}
	NFOUT;
	return rv;
}

static void free_zero_handler(struct per_disk *pd)
{
	NFIN;
	free_zero(pd, &pd->sw.zero);
	if (has_hw_zero(pd))
		free_zero(pd, &pd->hw->zero);
#if TEST_CODE
	free_zero(pd, &pd->sw.data);
	if (has_hw_zero(pd))
		free_zero(pd, &pd->hw->data);
#endif
	NFOUT;
}

static void free_pd(struct per_disk *pd)
{
	NFIN;
	free_reqs(pd);
	free_zero_handler(pd);
	if (has_hw_zero(pd)) {
		kfree(pd->hw);
		pd->hw = NULL;
	}
	NFOUT;
}

static int prepare_zero_handler_sec(struct per_disk *pd, struct zero_req *zreq,
	int data_int, int md_int, const char *str)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_pages = dsi->dt_n_pages + dsi->md_n_pages;
	int rv = -1;
	int i;

	NFIN;
	zreq->prpl_n_pages = DIV_ROUND_UP(n_pages * sizeof(dma_addr_t), PAGE_SIZE);
	_NT(trace_um_comm_prepare_zero_handler_sec, "disk @DISK_ID_STR, @STR.prpl_n_pages=@PRPL_N_PAGES",
		pd->di->disk_id, str, zreq->prpl_n_pages);
	if (prepare_zero(pd, zreq, data_int, md_int, str))
		goto out;
	for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i) {
		if ((rv = prepare_req(pd, zreq, &zreq->reqs[i]))) {
			goto freereqs;
		}
	}
	rv = 0;
	goto out;

freereqs:
	free_reqs(pd);

out:
	NFOUT;
	return rv;
}

static int prepare_zero_handler(struct per_disk *pd)
{
	int rv;

	NFIN;
	rv = prepare_zero_handler_sec(
		pd, &pd->sw.zero, DISK_DATA_INIT_BYTE, DISK_MD_INIT_BYTE, "zero");
#if TEST_CODE
	rv = rv || prepare_zero_handler_sec(
		pd, &pd->sw.data, DATA_INT, MD_INT, "data");
#endif
	if (has_hw_zero(pd)) {
		rv = rv || prepare_zero_handler_sec(
			pd, &pd->hw->zero, DISK_DATA_INIT_BYTE, DISK_MD_INIT_BYTE, "zero");
#if TEST_CODE
		rv = rv || prepare_zero_handler_sec(
			pd, &pd->hw->data, DATA_INT, MD_INT, "data");
#endif
	}
	NFOUT;
	return rv;
}

static int perpare_pd(struct per_disk *pd)
{
	int rv = -1;

	if (prepare_common(pd) || prepare_zero_handler(pd))
		goto error_pd;
	if (!list_empty(&pd->p->processes) &&
		send_disk_to_processes(pd->p, pd))
	{
		/* [NVMESH-4263] - Dont exit pd thread if send_disk_to_processes fails and
		 * toma is not up.Solves race where TOMA is connecting just as the drive is added.
		if (pd->p->toma_is_up)
			goto error_pd;
		*/
	}
	rv = 0;
	goto out;

error_pd:
	free_pd(pd);

out:
	NFOUT;
	return rv;
}

static bool handle_pd_nl_event(struct per_disk *pd, struct netlink_event *e);
static void drain_pd_events(struct per_disk *pd)
{
	struct nvmeib_public_kth_event *e;

	NFIN;
	if ((e = list_first_entry_or_null(
		&pd->um_events, struct nvmeib_public_kth_event, link))) {
		list_del_init(&e->link);
		handle_pd_nl_event(pd, e_to_nle(e));
		free_nle(e);
	}
	NFOUT;
}

static bool handle_pd_stop_event(struct per_disk *pd)
{
	bool cont;

	NFIN;
	pd->stopped = true;
	if (pd->in_progress)
		cont = true;
	else {
		drain_pd_events(pd);
		cont = false;
	}
	if (!cont)
		_NT(trace_um_comm_handle_pd_stop_event, "pd @DISK_ID_STR is now stopped...", pd->di->disk_id);
	NFOUT;
	return cont;
}

static void check_pd_cont_stop(struct per_disk *pd)
{
	NFIN;
	if (!pd->stopped) {
		_ND(trace_um_comm_check_pd_cont_stop, "Done with disk @DISK_ID_STR request and now fetch a new request...",
		   pd->di->disk_id);
		/* to avoid a recursion we post resume message to our own thread
		   so we fetch new events from the queue
		*/
		nvmeib_public_kth_resume(pd->base.kth);
	}
	else {
		/* drain the event queue */
		drain_pd_events(pd);
		_NT(trace_1_um_comm_check_pd_cont_stop, "We are done with disk @DISK_ID_STR", pd->di->disk_id);
		nvmeib_public_kth_stop(pd->base.kth);
	}
	NFOUT;
}

static void process_zero_init(struct per_disk *pd, struct zero_req *zreq,
	struct netlink_event *e, const char *str);
static void process_zero_check_zero_done(
	struct per_disk *pd, struct zero_req *zreq, const char *str)
{
	NFIN;
	if (!zreq->read_in_progress) {
		_ND(trace_um_comm_process_zero_check_zero_done, "No more reads in progress...");
		if (!zreq->stopped) {
			_ND(trace_1_um_comm_process_zero_check_zero_done, "Reply successful @STR...", str);
			reply_success_zero(pd->p, zreq, zreq->cur);
		}
		else {
			_ND(trace_2_um_comm_process_zero_check_zero_done, "Reply failed @STR...", str);
			reply_error_zero(pd->p, zreq, zreq->cur);
		}
		free_nle(&zreq->cur->e);
		zreq->stopped = false;
		zreq->cur = NULL;
		zreq->lba = 0;
		zreq->last_lba_plus_1 = 0;
		pd->in_progress = false;
		check_pd_cont_stop(pd);
	}
	NFOUT;
}

static void init_io_list(struct zero_req *zreq)
{
	int i;

	NFIN;
	INIT_LIST_HEAD(&zreq->r);
	for (i = 0; i < MAX_IO_CMD_IN_PROGRESS; ++i)
		list_add_tail(&zreq->reqs[i].link, &zreq->r);
	NFOUT;
}

static void clear_trim_op(struct per_disk *pd)
{
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	struct trim_op *t = &pd->trim_op;

	NFIN;
	if (t->dsm_buf) {
		if (!pd->di->external) {
			BUG_ON(!dev);
			dma_free_coherent(dev, sizeof(*t->dsm_buf), t->dsm_buf, t->dsm_buf_dma);
		} else {
			kfree(t->dsm_buf);
		}
		t->dsm_buf = 0;
		t->dsm_buf_dma = 0;
	}
	t->in_progress = false;
	t->e = NULL;
	t->zreq = NULL;
	NFOUT;
}

static void trim_disk(struct per_disk *pd,
	struct zero_req *zreq, struct netlink_event *e)
{
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	struct nvmeib_nl_uk_comm_msg *msg = e->p;
	struct nvmeib_zero_disk *zmsg;
	struct trim_op *t = &pd->trim_op;
	struct nvmeibs_nvme_req *req = &t->req;
	struct nvmeib_dsm_range *dsm_buf;
	dma_addr_t dsm_buf_dma = 0;
	int rv;

	NFIN;
	clear_trim_op(pd);
	if (!pd->di->external) {
		BUG_ON(!dev);
		dsm_buf = dma_alloc_coherent(dev, sizeof(*dsm_buf), &dsm_buf_dma, 0);
	} else {
		dsm_buf = kzalloc(sizeof(*dsm_buf), GFP_KERNEL);
	}

	if (!dsm_buf) {
		_NE(error_um_comm_trim_disk, "Failed to allocate trim buffer");
		goto rep_failed;
	}
	else {
		t->zreq = zreq;
		t->dsm_buf = dsm_buf;
		t->dsm_buf_dma = dsm_buf_dma;
		t->e = e;
	}
	zmsg = get_zero_disk(msg);
	dsm_buf->cattr = 0;
	dsm_buf->slba = zmsg->start_hw_sector;
	dsm_buf->nlb = zmsg->n_hw_sectors;
	req->nvme_op = nvme_cmd_dsm;
	req->buf_offset = 0;
	req->disk_block = 0;
	req->data_len = sizeof(struct nvmeib_dsm_range);
	req->n_dsm_lba = dsm_buf->nlb;
	if (!pd->di->external) {
		req->buf_addrs = &t->dsm_buf_dma;
		req->prpl_phys = dsm_buf_dma;
	} else {
		/* External drive, pass the ptr to submit_external_iop through the buf_addrs */
		req->buf_addrs = (void *)&t->dsm_buf;
	}
	_ND(trace_um_comm_trim_disk, "disk_block=@DISK_BLOCK, len=@LEN_LONG", (u64)zmsg->start_hw_sector, zmsg->n_hw_sectors);
	if ((rv = submit_local_cmd(pd->di, req)) < 0) {
		_ND(trace_1_um_comm_trim_disk, "Fail to submit trim cmd to disk @DISK_ID_STR (rv = @RV)",
			pd->di->disk_id, rv);
		/* we are done */
		goto rep_failed;
	}
	else {
		zreq->read_in_progress = 1;
		t->in_progress = true;
		goto out;
	}

rep_failed:
	clear_trim_op(pd);
	zreq->stopped = true;

out:
	NFOUT;
}

static int process_zero_read_start(
	struct per_disk *pd, struct zero_req *zreq, struct io_req *io);
static void process_zero_init(struct per_disk *pd, struct zero_req *zreq,
	struct netlink_event *e, const char *str)
{
	struct nvmeib_nl_uk_comm_msg *msg = e->p;
	struct nvmeib_zero_disk *zmsg;
	struct io_req *io;

	NFIN;
	zmsg = get_zero_disk(msg);
	zreq->cur = e;
	zreq->lba = zmsg->start_hw_sector;
	zreq->last_lba_plus_1 = zmsg->start_hw_sector + zmsg->n_hw_sectors;
	init_io_list(zreq);
	_ND(trace_um_comm_process_zero_init, "Start a new @STR request for disk @DISK_ID_STR - slba=@SLBA_LONG, len=@LEN_LONG", str,
		pd->di->disk_id, zmsg->start_hw_sector, zmsg->n_hw_sectors);
	if (zmsg->is_using_nvme_trim_before_zero) {
		_NT(trace_1_um_comm_process_zero_init, "@DISK_ID_STR: trim: disk trim is mandatory", pd->di->disk_id);
	}
	else {
		_NT(trace_2_um_comm_process_zero_init, "@DISK_ID_STR: trim: disk trim is NOT mandatory", pd->di->disk_id);
	}
	if (zmsg->is_zeroing_mandatory) {
		_NT(trace_3_um_comm_process_zero_init, "@DISK_ID_STR: trim: zeroing disk is mandatory", pd->di->disk_id);
		if (zmsg->is_zeroing_using_test_and_write) {
			_NT(trace_4_um_comm_process_zero_init, "@DISK_ID_STR: trim: zeroing disk by read and write", pd->di->disk_id);
		}
		else {
			_NT(trace_5_um_comm_process_zero_init, "@DISK_ID_STR: trim: zeroing disk by write", pd->di->disk_id);
		}
	}
	else {
		_NT(trace_6_um_comm_process_zero_init, "@DISK_ID_STR: trim: zeroing disk is NOT mandatory", pd->di->disk_id);
	}
	if (msg->opcode == csc_zero_disk) {
		if (zmsg->is_using_nvme_trim_before_zero) {
			trim_disk(pd, zreq, e);
			goto done;
		}
	}
	if (zmsg->is_zeroing_mandatory) {
		while ((io = list_first_entry_or_null(&zreq->r, struct io_req, link))) {
			list_del(&io->link);
			if (process_zero_read_start(pd, zreq, io))
				break;
		}
	}

done:
	process_zero_check_zero_done(pd, zreq, str);
	NFOUT;
}

static bool check_zero_msg(struct per_disk *pd, struct nvmeib_zero_disk *msg)
{
	struct disk_sector_info *dsi = !msg->is_hw ? &pd->sw : pd->hw;
	bool ok;

	NFIN;
	if (dsi->hw_md_size == 0) {
		ok = true;
		goto out;
	}
	if ((msg->start_hw_sector / dsi->n_hw_in_sw) * dsi->n_hw_in_sw !=
		msg->start_hw_sector) {
		_NE(error_um_comm_check_zero_msg, "Zero request on disk @DISK_ID_STR start_hw_lba @START_HW_SECTOR is a NOT "
		   "multiplication of n_hw_in_sw @N_HW_IN_SW (is_hw=@IS_HW)",
			pd->di->disk_id,
			msg->start_hw_sector, dsi->n_hw_in_sw, msg->is_hw ? 'T' : 'F');
		ok = false;
	}
	else if ((msg->n_hw_sectors / dsi->n_hw_in_sw) * dsi->n_hw_in_sw !=
			 msg->n_hw_sectors) {
		_NE(error_1_um_comm_check_zero_msg, "Zero request on disk @DISK_ID_STR n_hw_sectors @N_HW_SECTORS is a NOT "
		   "multiplication of n_hw_in_sw @N_HW_IN_SW (is_hw=@IS_HW)",
			pd->di->disk_id,
			msg->n_hw_sectors, dsi->n_hw_in_sw, msg->is_hw ? 'T' : 'F');
		ok = false;
	}
	else
		ok = true;
out:
	NFOUT;
	return ok;
}

static bool handle_zero_type_msg(struct per_disk *pd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg = e->p;
	struct nvmeib_zero_disk *zmsg;
	struct disk_sector_info *dsi;
	bool delete_msg;

	NFIN;
	pd->in_progress = true;
	zmsg = get_zero_disk(msg);
	if (!zmsg || !check_zero_msg(pd, zmsg)) {
		_NE(error_um_comm_handle_zero_type_msg, "Reject user zero request for disk @DISK_ID_STR - bad parameters",
			pd->di->disk_id);
		reply_error_code(pd->p, e, csce_bad_zero_params);
		pd->in_progress = false;
		delete_msg = true;
	}
	else {
		dsi = !zmsg->is_hw ? &pd->sw : pd->hw;
		_NT(trace_um_comm_handle_zero_type_msg, "Disk @DISK_ID_STR: asked for @TYPE_STR and using @TYPE_STR sectors for zeroing: "
		   "slba=@SLBA_LONG, n_hw_sec=@N_HW_SEC (e=@EVENT_PTR)",
			pd->di->disk_id,
			zmsg->is_hw ? "HW" : "SW", has_hw_zero(pd) ? "HW" : "SW",
			zmsg->start_hw_sector, zmsg->n_hw_sectors, e);
		if (msg->opcode == csc_zero_disk ||
			msg->opcode == csc_test_zero_disk) {
			dsi->zero.is_test_zero = msg->opcode == csc_test_zero_disk;
			process_zero_init(pd, &dsi->zero, e, "zero");
		}
#if TEST_CODE
		else {
			dsi->percentage_zero = ((struct nvmeib_contaminate_disk *)
									msg->data)->percentage_zero;
			process_zero_init(pd, &dsi->data, e, "data");
		}
#endif
		delete_msg = false;
	}
	NFOUT;
	return delete_msg;
}

static void clear_io_op(struct per_disk *pd, struct io_op *w)
{
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	int i;

	NFIN;
	if (w->vaddr) {
		nvmeib_public_vunmap(w->vaddr);
		w->vaddr = 0;
	}
	if (w->pages) {
		for (i = 0; i < w->n_pages; ++i) {
			if (!pd->di->external) {
				BUG_ON(!dev);
				if (w->prpl_virt[i] && !dma_mapping_error(dev, w->prpl_virt[i])) {
					dma_unmap_page(dev, w->prpl_virt[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
				}
			}
			if (w->pages[i]) {
				__free_pages(w->pages[i], 0);
				w->pages[i] = 0;
			}
		}
		kfree(w->pages);
		w->pages = 0;
	}
	if (w->prpl_virt) {
		if (!pd->di->external) {
			BUG_ON(!dev);
			dma_free_coherent(dev, w->prpl_n_pages * PAGE_SIZE,
					  w->prpl_virt, w->prpl_phys);
			w->prpl_phys = 0;
		} else {
			free_pages_exact(w->prpl_virt, w->prpl_n_pages * PAGE_SIZE);
		}
		w->prpl_virt = 0;
	}
	w->md = NULL;
	w->md_dma = 0;
	w->n_sectors = w->n_pages = w->prpl_n_pages = 0;
	w->e = NULL;
	w->in_progress = false;
	w->si = NULL;
	NFOUT;
}

static struct io_op * get_io_op(struct per_disk *pd)
{
	struct io_op *io_op;

	NFIN;
	if ((io_op = list_first_entry_or_null(&pd->io_ops, struct io_op, link))) {
		list_del_init(&io_op->link);
		clear_io_op(pd, io_op);
	}
	else {
		io_op = kzalloc(sizeof(*io_op), GFP_KERNEL);
		prepare_io_op(pd, io_op);
	}
	NFOUT;
	return io_op;
}

static void put_io_op(struct per_disk *pd, struct io_op *io_op)
{
	list_add(&io_op->link, &pd->io_ops);
}

static int allocate_io_op(struct per_disk *pd, struct io_op *w, int data_len)
{
	struct device *dev = get_nvme_dma_device(pd->di->dev);
	int i;
	int rv;

	NFIN;
	w->n_pages = DIV_ROUND_UP(data_len, PAGE_SIZE);
	/* we add in the case of MD: 1 for the larger size of the buffer
	   and one for reading the user MD data
	*/
	if (pd->di->metadata)
		w->n_pages += pd->di->mtdt_extd ? 2 : 1;
	w->prpl_n_pages = DIV_ROUND_UP(w->n_pages * sizeof(dma_addr_t), PAGE_SIZE);\
	if (!pd->di->external) {
		BUG_ON(!dev);
		w->prpl_virt = dma_alloc_coherent(
			dev, w->prpl_n_pages * PAGE_SIZE, &w->prpl_phys, GFP_KERNEL | __GFP_ZERO);
	} else {
		w->prpl_virt = (void *)alloc_pages_exact(w->prpl_n_pages * PAGE_SIZE, GFP_KERNEL | __GFP_ZERO);
	}
	if (!w->prpl_virt) {
		_NE(error_um_comm_allocate_io_op, "Failed to allocate prpl for pd @DISK_ID_STR", pd->di->disk_id);
		goto error;
	}
	if (!(w->pages = kcalloc(w->n_pages, sizeof(*w->pages), GFP_KERNEL))) {
		_NE(error_1_um_comm_allocate_io_op, "Failed to allocate page tables for io_op pd @DISK_ID_STR",
		   pd->di->disk_id);
		goto error;
	}
	for (i = 0; i < w->n_pages; ++i) {
		if (!(w->pages[i] = alloc_pages_node(dev ? dev->numa_node : NUMA_NO_NODE, GFP_KERNEL, 0))) {
			_NE(error_2_um_comm_allocate_io_op, "Failed to allocate page @PAGE_NUM for io_op pd @DISK_ID_STR",
			   i, pd->di->disk_id);
			goto error;
		}
		if (!pd->di->external) {
			BUG_ON(!dev);
			w->prpl_virt[i] = dma_map_page(dev, w->pages[i], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, w->prpl_virt[i])) {
				_NE(error_4_um_comm_allocate_io_op, "Failed to map page @PAGE_NUM for pd @DISK_ID_STR", i, pd->di->disk_id);
				goto error;
			}
		} else {
			/* External device, nvme layer will translate virtual addresses to bio pages */
			w->prpl_virt[i] = (unsigned long)page_address(w->pages[i]);
		}
	}
	/* Sync prpl with device */
	if (!pd->di->external)
		dma_sync_single_for_device(dev, w->prpl_phys, w->prpl_n_pages * PAGE_SIZE, DMA_TO_DEVICE);
	if (!(w->vaddr = vmap(w->pages, w->n_pages, VM_MAP, PAGE_KERNEL))) {
		_NE(error_3_um_comm_allocate_io_op, "Fail to vmap virt area for io_op pd @DISK_ID_STR", pd->di->disk_id);
		goto error;
	}
	w->md = page_address(w->pages[w->n_pages - 1]);
	w->md_dma = w->prpl_virt[w->n_pages - 1];
	memset(w->md, DISK_MD_INIT_BYTE, PAGE_SIZE);
	rv = 0;
	goto out;

error:
	clear_io_op(pd, w);
	put_io_op(pd, w);
	rv = -1;

out:
	NFOUT;
	return rv;
}

static int prepare_write_buffer(struct per_disk *pd, struct io_op *w)
{
	struct nvmeib_nl_uk_comm_msg *msg = w->e->p;
	struct nvmeib_io_to_disk *wmsg = (struct nvmeib_io_to_disk *)msg->data;
	void *p = w->vaddr;
	void *q = w->md;
	void *t;
	void *b;
	int b_len;
	int i, rv;

	NFIN;
	/* if the MD buffer arrived is less that the sw_sector MD size
	   we need to copy the arrived MD to a dummy buffer and then copy it back
	   to the buffer that goes to disk but this time adjusted
	*/
	b_len = w->md_size < w->si->sw_md_size ?
		max((int)wmsg->md_len, w->si->sw_sector) : w->si->sw_sector;
	if (!(b = kmalloc(b_len, GFP_KERNEL))) {
		_NE(error_um_comm_prepare_write_buffer, "Failed to allocate dummy buffer...");
		rv = -1;
		goto out;
	}
	if (w->md_size && w->md_size < w->si->sw_md_size) {
		_ND(trace_um_comm_prepare_write_buffer, "Adjusting MD from given size of @MD_LEN to disk size of @SW_MD_SIZE",
			wmsg->md_len, w->si->sw_md_size);
		memcpy(b, q, wmsg->md_len);
		memset(q, 0, PAGE_SIZE);
		for (i = 0; i < w->n_sectors; ++i)
			memcpy(q + i * w->si->sw_md_size, b + i * w->md_size, w->md_size);
		memset(b, 0, b_len);
	}
	p += w->n_sectors * w->si->sw_sector;
	q += w->n_sectors * w->si->sw_md_size;
	t = p + w->n_sectors * w->si->sw_md_size;
	while (true) {
		t -= w->si->sw_md_size;
		q -= w->si->sw_md_size;
		memcpy(t, q, w->si->sw_md_size);
		t -= w->si->sw_sector;
		p -= w->si->sw_sector;
		if (t != p) {
			memcpy(b, p, w->si->sw_sector);
			memcpy(t, b, w->si->sw_sector);
		}
		else
			break;
	}
	rv = 0;

out:
	kfree(b);
	NFOUT;
	return rv;
}

static void post_io(void *arg, int status, u32 result)
{
	struct pd_io_event *e = arg;

	NFIN;
	e->status = status;
	e->result = result;
	_ND(trace_um_comm_post_io, "Done pd io for pd of disk @DISK_ID_STR", e->pd->di->disk_id);
	if (nvmeib_public_kth_add_event(&e->e))
		_NE(error_um_comm_post_io, "No recipient for io message @TOPOLOGY_INT", e->e.id.t);
	NFOUT;
}

static int do_io(
	struct per_disk *pd, struct io_op *w, struct nvmeib_io_to_disk *wmsg)
{
	struct nvmeibs_nvme_req *req = &w->req;
	int rv;

	NFIN;
	req->nvme_op = wmsg->is_read ? nvme_cmd_read : nvme_cmd_write;
	req->buf_offset = 0;
	req->disk_block = wmsg->start_sector * w->si->n_hw_in_sw;
	req->use_hw_blocks = wmsg->is_hw;
	req->data_len = wmsg->data_len;
	req->buf_addrs = (dma_addr_t *)w->prpl_virt;
	req->prpl_phys = w->prpl_phys;
	_ND(trace_um_comm_do_io, "disk_block=@DISK_BLOCK, len=@LEN",
		(unsigned long)((u64)wmsg->start_sector * w->si->n_hw_in_sw), wmsg->data_len);
	if (pd->di->mtdt_extd) {
		req->metadata = NULL;
		req->mtdt_size = 0;
		_ND(trace_1_um_comm_do_io, "Inline MD: ");
	}
	else if (pd->di->metadata) {
		req->metadata = w->md;
		req->mtdt_dma_ptr = w->md_dma;
		req->mtdt_size = w->n_sectors * w->si->sw_md_size;
		_ND(trace_2_um_comm_do_io, "Sep MD: metadata=@METADATA, mtdt_size=@MTDT_SIZE_LONG",
			req->metadata, req->mtdt_size);
	}
	else {
		req->metadata = NULL;
		req->mtdt_size = 0;
		_ND(trace_3_um_comm_do_io, "No MD: ...");
	}
	if ((rv = submit_local_cmd(pd->di, req)) < 0) {
		_ND(trace_4_um_comm_do_io, "Fail to submit @DIRECTION_STR cmd to disk @DISK_ID_STR (rv = @RV)",
			wmsg->is_read ? "read" : "write", pd->di->disk_id, rv);
		/* we are done */
		rv = -1;
	}
	else
		rv = 0;
	NFOUT;
	return rv;
}

static int do_write(
	struct per_disk *pd, struct io_op *w, struct nvmeib_io_to_disk *wmsg)
{
	int rv = -1;

	NFIN;
	if (nvmeib_public_copy_user_pages(
		w->vaddr, wmsg->pid, wmsg->data, wmsg->data_len, 0) < 0)
		goto out;
	if (pd->di->metadata) {
		if (wmsg->md) {
			if (nvmeib_public_copy_user_pages(w->md,
					wmsg->pid, wmsg->md, wmsg->md_len, 0) < 0)
				goto out;
		}
		if (pd->di->mtdt_extd && prepare_write_buffer(pd, w) < 0)
			goto out;
	}
	rv = do_io(pd, w, wmsg);

out:
	NFOUT;
	return rv;
}

static int prepare_read_buffer(
	struct per_disk *pd, struct io_op *w, struct nvmeib_io_to_disk *wmsg)
{
	void *p = w->vaddr;
	void *q = wmsg->data;
	void *t;
	void *b = NULL;
	int md_size = 0;
	int i, rv = 0;

	NFIN;
	/* if the disk has MD */
	if (pd->di->metadata) {
		/* disk with separate MD */
		if (!pd->di->mtdt_extd) {
			/* if the caller requested the MD copy it and then if copy is
			   good jump and copy the date
			*/
			if (!wmsg->md ||
				!(rv = nvmeib_public_copy_user_pages(w->md,
					wmsg->pid, wmsg->md, wmsg->md_len, 1)))
				goto copy_data;
		}
		else if (pd->di->mtdt_extd) { /* disk with inline MD */
			/* allocate a dummy buffer for the MD */
			if (wmsg->md &&
				!(b = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO))) {
				_NE(error_um_comm_prepare_read_buffer, "Failed to allocate dummy buffer...");
				rv = -1;
				goto out;
			}
			else
				t = b;
			/* copy data sectors to the user data buffers and the MD if needed
			   to the dummy buffer
			*/
			md_size = min(w->md_size, w->si->sw_md_size);
			for (i = 0; i < w->n_sectors; ++i) {
				if ((rv = nvmeib_public_copy_user_pages(
					p, wmsg->pid, q, w->si->sw_sector, 1)))
					goto out;
				p += w->si->sw_sector;
				q += w->si->sw_sector;
				if (b) {
					memcpy(t, p, md_size);
					t += md_size;
				}
				p += w->si->sw_md_size;
			}
			/* copy the dummy buffer, if there is one to the user MD buffer */
			if (b) {
				if ((rv = nvmeib_public_copy_user_pages(b, wmsg->pid, wmsg->md,
					w->n_sectors * w->md_size, 1)))
					goto out;
			}
		}
	}
	else
copy_data:
		rv = nvmeib_public_copy_user_pages(
			w->vaddr, wmsg->pid, wmsg->data, wmsg->data_len, 1);

out:
	if (b)
		free_page((unsigned long)b);
	NFOUT;
	return rv;
}

static bool handle_io_msg(struct per_disk *pd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg = e->p;
	struct nvmeib_io_to_disk *wmsg;
	struct io_op *w;
	struct disk_sector_info *si;
	bool delete_msg;
	const void __user *gpt_ents = NULL;
	size_t gpt_ents_len = 0;
	bool serjio_gpt_update = false;
	enum nvmeib_main_gpt_update_flags gpt_update_stage;
	int rv;

	NFIN;
	pd->in_progress = true;
	if (!(w = get_io_op(pd))) {
		_NT(trace_um_comm_handle_io_msg_111,
			"Failed to get io_op for disk io request.");
		goto rep_failed;
	}
	clear_io_op(pd, w);
	wmsg = (struct nvmeib_io_to_disk *)msg->data;
	si = wmsg->is_hw ? pd->hw : &pd->sw;
	if (wmsg->data_len <= 0)
		goto invalid_param;
	if (wmsg->data_len % si->sw_sector)
		goto invalid_param;
	if (!wmsg->data || !!((unsigned long)wmsg->data & (PAGE_SIZE - 1)))
		goto invalid_param;
	if (!wmsg->md_len && wmsg->md)
		goto invalid_param;
	gpt_update_stage = wmsg->gpt_update_flags & MAIN_GPT_UPDATE_STAGE_MASK;
	if (gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE ||
		gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE) {
		/* Check MD len */
		if (!wmsg->md || !wmsg->md_len)
			goto invalid_param;
		if (wmsg->start_sector != 1 && wmsg->start_sector != pd->di->hw_blocks - 1)
			goto invalid_param;

		gpt_ents = wmsg->md;
		gpt_ents_len = wmsg->md_len;

		if (!gpt_ents || gpt_ents_len == 0)
			goto invalid_param;

		wmsg->md = NULL;
		wmsg->md_len = 0;
	}
	else if (wmsg->md_len) {
		if (!wmsg->md)
			goto invalid_param;
		if (wmsg->md_len < DISK_MIN_MD_SIZE_BYTE * (wmsg->data_len / si->sw_sector))
			goto invalid_param;
		if (wmsg->md_len > si->sw_md_size * (wmsg->data_len / si->sw_sector))
			goto invalid_param;
	}

	w->e = e;
	w->si = si;
	w->n_sectors = wmsg->data_len / si->sw_sector;
	w->md_size = wmsg->md_len / w->n_sectors;
		_NT(trace_um_comm_handle_io_msg, "IO op is @DIRECTION_STR, IO is by @TYPE_STR sector, n_sectors=@N_SECTORS, md_size=@MD_SIZE",
			wmsg->is_read ? "Read" : "Write", wmsg->is_hw ? "HW" : "SW",
			w->n_sectors, w->md_size);
	_NT(handle_io_msg_t1, "IO op is @STR, IO is by @STR sector, start_sector=@LU, n_sectors=@INT, md_size=@INT",
		wmsg->is_read ? "Read" : "Write", wmsg->is_hw ? "HW" : "SW",
		wmsg->start_sector, w->n_sectors, w->md_size);
	if (gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE ||
		gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE) {
		bool is_primary = wmsg->start_sector == 1;
		/* Check with SERJIO that is is ready for the GPT update */
		BUG_ON(wmsg->is_read);
		BUG_ON(!gpt_ents);
		_NT(handle_io_msg_t2, "GPT update - disk @STR, @STR", pd->di->disk_id,
		   nvmeib_gpt_update_str(wmsg->gpt_update_flags, is_primary));
		if ((rv = nvmeibs_serjio_gpt_update(pd->di, is_primary, wmsg->gpt_update_flags,
			wmsg->pid, wmsg->data, wmsg->data_len, gpt_ents, gpt_ents_len)) < 0) {
			_NW(handle_io_msg_t1_srj_gpt_fail, "nvmeibs_serjio_gpt_update failed (@RV) for disk @STR", rv, pd->di->disk_id);
			goto rep_failed;
		}
		serjio_gpt_update = true;
	}
	if (allocate_io_op(pd, w, wmsg->data_len) < 0)
		goto rep_failed;
	_ND(trace_1_um_comm_handle_io_msg, "IO op is @DIRECTION_STR, data=@DATA_PTR, data_len=@DATA_LEN, md=@MD_PTR, md_len=@MD_LEN",
		wmsg->is_read ? "Read" : "Write",
		wmsg->data, wmsg->data_len, wmsg->md, wmsg->md_len);
	if ((wmsg->is_read ? do_io(pd, w, wmsg) : do_write(pd, w, wmsg)))
		goto rep_failed;
	else {
		w->in_progress = true;
		delete_msg = false;
		goto out;
	}

invalid_param:
	_NE(handle_io_msg_e1, "Invalid IO parameters (PID @INT): start_sector=@LONG, data=@PTR, data_len=@INT, "
		"md=@PTR, md_len=@INT, gpt_update_flags: @INT32_HEX",
		wmsg->pid, wmsg->start_sector, wmsg->data, wmsg->data_len,
		wmsg->md, wmsg->md_len, wmsg->gpt_update_flags);

rep_failed:
	if (serjio_gpt_update) {
		/* Tell SERJIO GPT Update Failed */
		nvmeibs_serjio_gpt_update_done(pd->di,
			wmsg->start_sector == 1, wmsg->gpt_update_flags, -EIO);
	}
	reply_error_code(pd->p, e, csce_io_failed);
	if (w) {
		clear_io_op(pd, w);
		put_io_op(pd, w);
	}
	delete_msg = true;
	pd->in_progress = false;
	check_pd_cont_stop(pd);

out:
	NFOUT;
	return delete_msg;
}

static void copy_data(void *src, int src_len, void *priv)
{
	struct identify_exe *ie = priv;
	struct nvmeib_nl_uk_comm_msg *msg = ie->e->p;
	struct nvmeib_identify_disk *id = (struct nvmeib_identify_disk *)msg->data;

	NFIN;
	if ((ie->data_len = nvmeib_public_copy_user_pages(
		src, id->pid, id->data, src_len, 1)) < 0)
		_NE(copy_data_e1, "Failed to copy identified data to user @INT", ie->data_len);
	else
		ie->data_len = src_len;
	NFOUT;
}

static int ie_run(void *arg)
{
	struct identify_exe *ie = arg;
	struct per_disk *pd = container_of(ie, struct per_disk, ie);
	int rv;

	NFIN;
	_NT(ie_run_t1, "Started identify thread for disk @STR...", pd->di->disk_id);
	nvmeib_public_kth_switch_current_eq(&ie->base.events);
	nvmeib_public_kth_wait_resume();
	rv = nvmeibs_nvme_identify_disk(pd->di, copy_data, ie);
	_NT(ie_run_t2, "Identify thread for disk @STR @STR...",
		pd->di->disk_id, rv == 0 ? "finished successfully" : "failed");
	NFOUT;
	return -1;
}

static int ie_done(void *arg)
{
	struct identify_exe *ie = arg;

	NFIN;
	nvmeib_public_kth_add_event(&ie->end.e);
	NFOUT;
	return 0;
}

#define IDENTIFY_MIN_BUFFER_SIZE 4096
static int handle_identify_msg(struct per_disk *pd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_identify_disk *id;
	struct identify_exe *ie = &pd->ie;
	char name[72];
	struct nvmeib_public_kth_params params = {
		.name = name,
		.short_name = proc_name_format("S", "UK", "idpd"),
		.cpu = -1, /* dont care */
		.started = false,
		.is_main = true,
		.run = ie_run,
		.done = ie_done,
	};
	struct nvmeib_public_kth_id kth;
	bool delete_msg;
	int rv;

	NFIN;
	pd->in_progress = true;
	msg = e->p;
	id = (struct nvmeib_identify_disk *)msg->data;
	if (id->data_len < IDENTIFY_MIN_BUFFER_SIZE) {
		_NE(handle_identify_msg_e1, "Identify request provided a small reply buffer: needed at least @INT,"
		   " provided @INT", IDENTIFY_MIN_BUFFER_SIZE, id->data_len);
		goto rep_failed;
	}
	_ND(handle_identify_msg_d1, "Identify disk from @STR", pd->di->disk_id);
	ie->e = e;
	params.run_arg = ie;
	params.done_arg = ie;
	nvmeib_public_kth_obj_init(&ie->base);
	snprintf(name, sizeof(name), "S~%s", pd->di->disk_id);
	if (nvmeib_public_kth_is_err(
		(kth = nvmeib_public_kth_create(&params)))) {
		_NE(handle_identify_msg_e2, "Fail to create identify executor for @STR", pd->di->disk_id);
		rv = csce_identify_thread;
	}
	else {
		ie->base.kth = kth;
		if ((rv = nvmeib_public_kth_resume(get_kth(ie)))) {
			_NE(handle_identify_msg_e3, "Failed to resume identify kth of disk @STR - @INT",
				pd->di->disk_id, rv);
			goto rep_failed;
		}
		else {
			delete_msg = false;
			goto out;
		}
	}

rep_failed:
	reply_error_code(pd->p, e, csce_disk_stopped);
	delete_msg = true;
	pd->in_progress = false;
	check_pd_cont_stop(pd);

out:
	NFOUT;
	return delete_msg;
}

static bool handle_pd_nl_event(struct per_disk *pd, struct netlink_event *e)
{
	struct nvmeib_nl_uk_comm_msg *msg = e->p;
	bool delete_msg;

	NFIN;
	if (pd->stopped) {
		_NE(error_um_comm_handle_pd_nl_event, "Reject user zero request for disk @DISK_ID_STR - stopped", pd->di->disk_id);
		reply_error_code(pd->p, e, csce_disk_stopped);
		delete_msg = true;
	}
	else if (msg->opcode == csc_zero_disk || msg->opcode == csc_test_zero_disk
#if TEST_CODE
		|| msg->opcode == csc_contaminate_disk
#endif
		)
		delete_msg = handle_zero_type_msg(pd, e);
	else if (msg->opcode == csc_io_to_disk)
		delete_msg = handle_io_msg(pd, e);
	else if (msg->opcode == csc_identify_disk)
		delete_msg = handle_identify_msg(pd, e);
	else {
		_NE(error_1_um_comm_handle_pd_nl_event, "Unsupported user-mode to kernel message @OPCODE", msg->opcode);
		reply_error_code(pd->p, e, csce_unsupported_opcode);
		delete_msg = true;
	}
	NFOUT;
	return delete_msg;
}

static void handle_pd_resume(struct per_disk *pd)
{
	struct nvmeib_public_kth_event *e, *e_prev = NULL;

	NFIN;
	if (!pd->in_progress) {
		while ((e = list_first_entry_or_null(
			&pd->um_events, struct nvmeib_public_kth_event, link))) {
			if (e == e_prev)
				break;
			e_prev = e;

			list_del_init(&e->link);
			if (handle_pd_nl_event(pd, e_to_nle(e)))
				free_nle(e);
//			else
//				break;
		}

	}
	NFOUT;
}

static bool handle_pd_nl(struct per_disk *pd, struct netlink_event *e)
{
	bool delete_msg;

	NFIN;
	/* we must check that the we are in_progress and that the list is empty.
	   It is valid for the in_progress and non_empty list when we just
	   finish handling a request and we posted resume and at the same time
	   a new request snicked into message queue.
	*/
	if (!pd->in_progress && list_empty(&pd->um_events)) {
		delete_msg = handle_pd_nl_event(pd, e);
	}
	else {
		_ND(trace_um_comm_handle_pd_nl, "Queuing NL message, pd=@PD e=@EVENT_PTR pd->in_progress=@IN_PROGRESS list_empty=@LIST_EMPTY", pd, e, pd->in_progress, list_empty(&pd->um_events));
		list_add_tail(&e->e.link, &pd->um_events);
		delete_msg = false;
	}
	NFOUT;
	return delete_msg;
}

static void handle_io_end(struct per_disk *pd, struct pd_io_event *e)
{
	struct io_op *io = e_to_ioop(e);
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_io_to_disk *wmsg;
	enum nvmeib_main_gpt_update_flags gpt_update_stage;
	int rv;

	NFIN;
	_ND(trace_um_comm_handle_io_end, "io->in_progress=@IN_PROGRESS_CHR", io->in_progress ? 'Y' : 'N');
	if (io->in_progress) {
		_ND(trace_1_um_comm_handle_io_end, "e->status=@STATUS", e->status);
		msg = io->e->p;
		wmsg = (struct nvmeib_io_to_disk *)msg->data;
		gpt_update_stage = wmsg->gpt_update_flags & MAIN_GPT_UPDATE_STAGE_MASK;
		if (gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE ||
			gpt_update_stage == MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE) {
			/* Inform SERJIO that IO was successful */
			if ((rv = nvmeibs_serjio_gpt_update_done(pd->di, wmsg->start_sector == 1,
							wmsg->gpt_update_flags, e->status)) < 0)
			{
				_NT(trace_um_comm_handle_io_end_serjio_fail, "nvmeibs_serjio_gpt_update_done failed (@RV)", rv);
				if (!e->status)
					e->status = rv;
			}
		}
		if (!e->status) {
			_NT(trace_2_um_comm_handle_io_end, "e->status=@STATUS", e->status);
			if (wmsg->is_read) {
				if (!prepare_read_buffer(pd, io, wmsg))
					reply_success_io(pd->p, io->e);
				else
					reply_error_code(pd->p, io->e, csce_io_failed);
			}
			else {
				reply_success_io(pd->p, io->e);
			}
		}
		else
			reply_error_code(pd->p, io->e, csce_io_failed);
		free_nle(&io->e->e);
		clear_io_op(pd, io);
		put_io_op(pd, io);
		pd->in_progress = false;
	}
	check_pd_cont_stop(pd);
	NFOUT;
}

static void handle_trim_end(struct per_disk *pd, struct pd_io_event *e)
{
	struct trim_op *t = &pd->trim_op;
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_zero_disk *zmsg;
	struct zero_req *zreq;
	bool freenl = true;

	NFIN;
	_ND(trace_um_comm_handle_trim_end, "trim->in_progress=@IN_PROGRESS_CHR", t->in_progress ? 'Y' : 'N');
	if (!t->in_progress)
		goto out;

	zreq = t->zreq;
	zreq->read_in_progress = 0;
	_ND(trace_1_um_comm_handle_trim_end, "e->status=@STATUS", e->status);
	msg = t->e->p;
	zmsg = get_zero_disk(msg);
	pd->in_progress = false;
	if (zmsg->is_zeroing_mandatory) {
		_NT(trace_2_um_comm_handle_trim_end, "@DISK_ID_STR: trim: trim ended and zero by @TYPE_STR", pd->di->disk_id,
			zmsg->is_zeroing_using_test_and_write ? "read and write" : "write");
		zmsg->is_using_nvme_trim_before_zero = 0;
		/* we must call the real zero */
		_ND(trace_3_um_comm_handle_trim_end, "Calling the real zero after trim: must_zero=@MUST_ZERO",
			zmsg->is_zeroing_using_test_and_write ? 'T' : 'F');
		freenl = handle_pd_nl_event(pd, t->e);
	}
	else {
		_NT(trace_4_um_comm_handle_trim_end, "@DISK_ID_STR: trim: trim ended and NO zero", pd->di->disk_id);
		pd->in_progress = false;
		if (!e->status) {
			_NT(trace_5_um_comm_handle_trim_end, "e->status=@STATUS", e->status);
			reply_success_zero(pd->p, zreq, t->e);
		}
		else
			reply_error_zero(pd->p, zreq, t->e);
	}
	if (freenl)
		free_nle(&t->e->e);
	clear_trim_op(pd);
	check_pd_cont_stop(pd);

out:
	NFOUT;
}

static void handle_identify_end(struct per_disk *pd)
{
	NFIN;
	if (pd->ie.data_len > 0)
		reply_success_identify(pd->p, pd->ie.e, &pd->ie);
	else
		reply_error_code(pd->p, pd->ie.e, csce_identify_failed);
	pd->in_progress = false;
	check_pd_cont_stop(pd);
	NFOUT;
}

static void handle_zero_read_end(
	struct per_disk *pd, struct zero_io_event *e);
static void handle_zero_write_end(
	struct per_disk *pd, struct zero_io_event *e);
static void wait_pd_events(struct per_disk *pd)
{
	struct nvmeib_public_kth_event *e;
	bool delete_msg;
	bool cont = true;

	NFIN;
	_NT(trace_um_comm_wait_pd_events, "Starting pd @DISK_ID_STR event loop", pd->di->disk_id);
	while (cont && !nvmeib_public_kth_obj_wait_events(&pd->base, -1, NULL)) {
		while (cont && (e = list_first_entry_or_null(&pd->base.events,
			struct nvmeib_public_kth_event, link))) {
			list_del(&e->link);
			_ND(wait_pd_events_d1, "Handling event @STR (@INT)",
				nvmeib_kth_event_to_str((int)e->type), (int)e->type);
			delete_msg = true;
			switch (e->type) {
			case nke_stop:
				cont = handle_pd_stop_event(pd);
				break;
			case nke_resume:
				handle_pd_resume(pd);
				break;
			case umc_nl:
				delete_msg = handle_pd_nl(pd, e_to_nle(e));
				break;
			case umc_zero_read:
				handle_zero_read_end(pd, e_to_zio(e));
				break;
			case umc_zero_write:
				handle_zero_write_end(pd, e_to_zio(e));
				break;
			case umc_pd_io:
				handle_io_end(pd, e_to_pdio(e));
				break;
			case umc_pd_trim:
				handle_trim_end(pd, e_to_pdio(e));
				break;
			case umc_identify_end:
				handle_identify_end(pd);
			default:
				break;
			}
			if (delete_msg)
				nvmeib_public_kth_event_free(e);
		}
	}
	NFOUT;
}

static void post_zero_read(void *arg, int status, u32 result)
{
	struct zero_io_event *e = arg;

	NFIN;
	e->status = status;
	e->result = result;
	_ND(trace_um_comm_post_zero_read, "Done read on io[@REQ_INDEX]", (int)(e->io - &e->zreq->reqs[0]));
	if (nvmeib_public_kth_add_event(&e->e))
		_NE(error_um_comm_post_zero_read, "No recipient for zero_read message @TOPOLOGY_INT", e->e.id.t);
	NFOUT;
}

static void post_zero_write(void *arg, int status, u32 result)
{
	struct zero_io_event *e = arg;

	NFIN;
	e->status = status;
	e->result = result;
	_ND(trace_um_comm_post_zero_write, "Done write on io[@REQ_INDEX]", (int)(e->io - &e->zreq->reqs[0]));
	if (nvmeib_public_kth_add_event(&e->e))
		_NE(error_um_comm_post_zero_write, "No recipient for zero_write message @TOPOLOGY_INT", e->e.id.t);
	NFOUT;
}

#if 0

static void post_zero_check_write(void *arg, int status, u32 result)
{
	struct zero_io_event *e = arg;

	NFIN;
	e->status = status;
	e->result = result;
	_ND(post_zero_check_write_d1, "Done read on io[@INT]", (int)(e->io - &e->zreq->reqs[0]));
	if (nvmeib_public_kth_add_event(&e->e))
		_NE(post_zero_check_write_e1, "No recipient for zero_read message @UINT", e->e.id.t);
	NFOUT;
}

static bool filter_check_write_event(struct nvmeib_public_kth_event *e)
{
	return e->type == nke_stop || e->type == umc_zero_check_write;
}

static int wait_check_write_events(struct per_disk *pd)
{
	struct nvmeib_public_kth_event *e;
	bool cont = true;
	int rv = 0;

	NFIN;
	_ND(wait_check_write_events_d1, "Waiting...");
	while (cont && !(rv = nvmeib_public_kth_wait_events(
		filter_check_write_event, false, &e))) {
		switch (e->type) {
		case nke_stop:
			rv = -1;
			cont = false;
			break;
		case umc_zero_check_write:
			cont = false;
			break;
		default:
			_NW(wait_check_write_events_w1, "Unexpected event @INT (@STR) - ignored",
				e->type, nvmeib_kth_event_to_str(e->type));
			break;
		}
		nvmeib_public_kth_event_free(e);
	}
	NFOUT;
	return rv;
}

static bool check_zd_write(struct per_disk *pd, struct io_req *io, int len,
	u8 data_int, u8 md_int)
{
	int n_sw_sectors;
	void *p;
	int i, j;
	u8 *q;
	bool ok = true;

	NFIN;
	n_sw_sectors = len / (pd->sw_sector + pd->sw_md_size);
	p = io->vaddr;
	for (i = 0; i < n_sw_sectors; ++i) {
		q = p;
		for (j = 0; j < pd->sw_sector; ++j) {
			if (*q != data_int) {
				_NE(check_zd_write_e1, "Check write failed - expected 0x@INT32_02, received 0x@INT32_02, "
				    "offset=@LX",
					data_int, *q, (void *)q - p);
				ok = false;
				goto out;
			}
		}
		p += pd->sw_sector;
		q = p;
		for (j = 0; j < pd->sw_md_size; ++j) {
			if (*q != md_int) {
				_NE(check_zd_write_e2, "Check write failed - expected 0x@INT32_02, received 0x@INT32_02, "
				   "offset=@LX",
					md_int, *q, (void *)q - p);
				ok = false;
				goto out;
			}
		}
		p += pd->sw_md_size;
	}

out:
	NFOUT;
	return ok;
}

static void chech_write(struct per_disk *pd, struct zero_req *zreq,
	struct io_req *io, struct zero_io_event *e)
{
	int index = e - &io->es[0];
	bool ok;
	int rv;

	NFIN;
	io->reqs[index].nvme_op = nvme_cmd_read;
	io->reqs[index].buf_addrs = (dma_addr_t *)io->prpl_virt;
	io->reqs[index].prpl_phys = io->prpl_phys;
	io->reqs[index].cb = post_zero_check_write;
	io->reqs[index].arg = e;
	e->e.type = umc_zero_check_write;
	if ((rv = submit_local_cmd(pd->di, &io->reqs[index])) < 0) {
		_ND(chech_write_d1, "Fail to submit cmd to disk @STR (rv = @INT)",
			pd->di->disk_id, rv);
		zreq->stopped = true;
	}
	else {
		if (!wait_check_write_events(pd)) {
			ok =  (zreq == &pd->zero) ?
				check_zd_write(pd, io, io->reqs[index].len, DISK_DATA_INIT_BYTE, DISK_MD_INIT_BYTE) :
				check_zd_write(pd, io, io->reqs[index].len, DATA_INT, MD_INT);
			if (!ok)
				zreq->stopped = true;
		}
		else
			zreq->stopped = true;
	}
	NFOUT;
}
#endif

static void handle_zero_write_end(
	struct per_disk *pd, struct zero_io_event *e)
{
	struct zero_req *zreq = e->zreq;
	struct disk_sector_info *dsi = zreq->dsi;
	const char *str = zreq == &dsi->zero ? "zero" : "data";
	int req_index = e - &e->io->es[0];
	struct nvmeibs_nvme_req *nvme_req = &e->io->reqs[req_index];

	NFIN;
	_ND(trace_um_comm_handle_zero_write_end, "Done write on io[@REQ_INDEX]", (int)(e->io - &e->zreq->reqs[0]));
	if (e->status) {
		_NE(error_um_comm_handle_zero_write_end,
			"Failed to write, (status=@STATUS, result=@RESULT_INT), "
			"io[@REQ_INDEX],"
		   "io->reqs[@REQ_INDEX].(disk_block=@DISK_BLOCK_LLONG, "
		   "data_len=@DATA_LEN_LONG)",
			e->status, e->result, (int)(e->io - &e->zreq->reqs[0]),
			req_index, (u64)nvme_req->disk_block, (unsigned long)nvme_req->data_len);
		zreq->stopped = true;
	}
	_ND(trace_1_um_comm_handle_zero_write_end, "write_in_progress=@WRITE_IN_PROGRESS, read_in_progress=@READ_IN_PROGRESS",
		e->io->write_in_progress, zreq->read_in_progress);
	if (!--e->io->write_in_progress)
		--zreq->read_in_progress;
#if 0
	if (!zreq->stopped) {
		chech_write(pd, zreq, e->io, e);
	}
#endif
	if (!e->io->write_in_progress && !zreq->stopped) {
		if (process_zero_read_start(pd, zreq, e->io)) {
			process_zero_check_zero_done(pd, zreq, str);
		}
	}
	else
		process_zero_check_zero_done(pd, zreq, str);
	NFOUT;
}

static int process_zero_write_start(struct per_disk *pd, struct zero_req *zreq,
	struct io_req *io, struct nvmeibs_nvme_req *req,
	unsigned long lba, unsigned long len)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int index = req - &io->reqs[0];
	int rv;

	NFIN;
	_ND(trace_um_comm_process_zero_write_start, "Start write on io[@REQ_INDEX], sw_lba=@SW_LBA", (int)(io - &zreq->reqs[0]), (u64)lba);
	req->nvme_op = nvme_cmd_write;
	req->buf_offset = 0;
	req->disk_block = lba;
	req->data_len = len;
	_ND(trace_1_um_comm_process_zero_write_start, "disk_block=@DISK_BLOCK, len=@LEN_LONG", (unsigned long)req->disk_block, req->data_len);
	req->buf_addrs = (dma_addr_t *)zreq->zero_req.prpl_virt;
	req->prpl_phys = zreq->zero_req.prpl_phys;
	_ND(trace_2_um_comm_process_zero_write_start, "buf_addrs=@BUF_ADDRS, prpl_phys=@PRPL_PHYS", req->buf_addrs, req->prpl_phys);
	if (pd->di->mtdt_extd) {
		req->metadata = NULL;
		req->mtdt_size = 0;
		_ND(trace_3_um_comm_process_zero_write_start, "Inline MD: ");
	}
	else if (pd->di->metadata) {
		req->metadata = zreq->zero_req.md;
		req->mtdt_dma_ptr = zreq->zero_req.md_dma;
		req->mtdt_size = (len / dsi->sw_sector) * dsi->sw_md_size;
		_ND(trace_4_um_comm_process_zero_write_start, "Sep MD: metadata=@METADATA, mtdt_size=@MTDT_SIZE_LONG",
			req->metadata, req->mtdt_size);
	}
	else {
		req->metadata = NULL;
		req->mtdt_size = 0;
		_ND(trace_5_um_comm_process_zero_write_start, "No MD: ...");
	}
	req->cb = post_zero_write;
	req->arg = &io->es[index];
	io->es[index].e.type = umc_zero_write;
	if ((rv = submit_local_cmd(pd->di, req)) < 0) {
		_ND(trace_6_um_comm_process_zero_write_start, "Fail to submit write cmd to disk @DISK_ID_STR (rv = @RV)",
			pd->di->disk_id, rv);
		zreq->stopped = true;
		/* we are done */
		if (!io->write_in_progress)
			--zreq->read_in_progress;
		_ND(trace_7_um_comm_process_zero_write_start, "write_in_progress=@WRITE_IN_PROGRESS, read_in_progress=@READ_IN_PROGRESS",
			io->write_in_progress, zreq->read_in_progress);
		rv = -1;
	}
	else {
		++io->write_in_progress;
		_ND(trace_8_um_comm_process_zero_write_start, "write_in_progress=@WRITE_IN_PROGRESS", io->write_in_progress);
		rv = 0;
	}
	NFOUT;
	return rv;
}

static bool is_buf_zero(void *in_buf, int len)
{
	unsigned long *buf = in_buf;
	unsigned char *buf_end = in_buf + len;
	unsigned long end = (unsigned long)(buf_end - (4 * sizeof(*buf)));
	unsigned long buf_or = 0;

	/* testing 4 unsigned_long per cycle */
	while ((unsigned long)buf <= end) {
		if (buf[0] | buf[1] | buf[2] | buf[3])
			return false;
		buf += 4;
	}
	in_buf = buf;
	while ((unsigned char *)in_buf < buf_end) {
		buf_or |= *((unsigned char *)in_buf);
		in_buf++;
	}
	return (buf_or == 0);
}

static void dump_sw_sector(void *s, int len)
{
	u8 *p = s;
	int i;

	NFIN;
	for (i = 0; i < len / 16; ++i) {
		_ND(trace_um_comm_dump_sw_sector, "@DB @DB @DB @DB @DB @DB @DB @DB "
			"@DB @DB @DB @DB @DB @DB @DB @DB",
			p[0], p[1], p[ 2], p[ 3], p[ 4], p[ 5], p[ 6], p[ 7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		p += 16;
	}
	NFOUT;
}

static void dump_sw_md(void *s, int len)
{
	u8 *p = s;
	int i;

	NFIN;
	for (i = 0; i < len / 8; ++i) {
		_ND(trace_um_comm_dump_sw_md, "@DB @DB @DB @DB @DB @DB @DB @DB",
			p[0], p[1], p[ 2], p[ 3], p[ 4], p[ 5], p[ 6], p[ 7]);
		p += 9;
	}
	NFOUT;
}

static void process_zero_write_start_inline(struct per_disk *pd,
	struct zero_req *zreq, struct io_req *req, const char *str)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_sw_sectors;
	void *p;
	void *q;
	unsigned long sw_lba;
	int i;
	bool is_zero;
	unsigned long write_lba = 0;
	int write_len = 0;
	int write_index = -1;

	NFIN;
	n_sw_sectors = req->reqs[0].data_len / dsi->sw_sector;
	p = req->vaddr;
	q = dsi->zero.zero_req.vaddr + dsi->sw_sector;
	sw_lba = req->reqs[0].disk_block;
	for (i = 0; i < n_sw_sectors; ++i, ++sw_lba) {
		is_zero =
			is_buf_zero(p, dsi->sw_sector) &&
			(memcmp(p + dsi->sw_sector, q, dsi->sw_md_size) == 0);
		if (zreq->is_test_zero) {
			if (!is_zero) {
				_NE(error_um_comm_process_zero_write_start_inline, "Test zero failed on disk @DISK_ID_STR @ sw_lba=@SW_LBA",
					pd->di->disk_id, (u64)sw_lba);
				dump_sw_sector(p, dsi->sw_sector);
				dump_sw_md(p + dsi->sw_sector, dsi->sw_md_size);
				zreq->failed_sw_lba = sw_lba;
				zreq->stopped = true;
				break;
			}
		}
		else if (zreq->need_write(dsi, is_zero)) {
			_ND(trace_um_comm_process_zero_write_start_inline, "sw_lba=@SW_LBA needs write", (u64)sw_lba);
			if (write_index < 0) {
				write_lba = sw_lba;
				write_len = 0;
				write_index = i;
			}
			write_len += dsi->sw_sector;
		}
		else {
			if (write_index > -1) {
				_ND(trace_1_um_comm_process_zero_write_start_inline, "sw_lba=@SW_LBA no need for write - "
				   "but we have pending writes", (u64)sw_lba);
				if (process_zero_write_start(pd, zreq, req,
					&req->reqs[write_index], write_lba, write_len)) {
					_NE(error_1_um_comm_process_zero_write_start_inline, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
					write_index = -1;
					break;
				}
				else {
					write_index = -1;
					write_lba = 0;
					write_len = 0;
				}
			}
		}
		p += dsi->sw_sector + dsi->sw_md_size;
	}
	if (write_index >= 0)
		if (process_zero_write_start(pd, zreq, req,
			&req->reqs[write_index], write_lba, write_len))
			_NE(error_2_um_comm_process_zero_write_start_inline, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
	NFOUT;
}

static void process_zero_write_start_sep(struct per_disk *pd,
	struct zero_req *zreq, struct io_req *req, const char *str)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_sw_sectors;
	void *pt;
	void *pm;
	void *q;
	unsigned long sw_lba;
	int i;
	bool is_zero;
	unsigned long write_lba = 0;
	int write_len = 0;
	int write_index = -1;

	NFIN;
	n_sw_sectors = req->reqs[0].data_len / dsi->sw_sector;
	pt = req->vaddr;
	pm = req->md;
	q = dsi->zero.zero_req.md;
	sw_lba = req->reqs[0].disk_block;
	for (i = 0; i < n_sw_sectors; ++i, ++sw_lba) {
		is_zero =
			is_buf_zero(pt, dsi->sw_sector) &&
			(memcmp(pm, q, dsi->sw_md_size) == 0);
		if (zreq->is_test_zero) {
			if (!is_zero) {
				_NE(error_um_comm_process_zero_write_start_sep, "Test zero failed on disk @DISK_ID_STR @ sw_lba=@SW_LBA",
					pd->di->disk_id, (u64)sw_lba);
				zreq->failed_sw_lba = sw_lba;
				zreq->stopped = true;
				break;
			}
		}
		else if (zreq->need_write(dsi, is_zero)) {
			_ND(trace_um_comm_process_zero_write_start_sep, "sw_lba=@SW_LBA needs write", (u64)sw_lba);
			if (write_index < 0) {
				write_lba = sw_lba;
				write_len = 0;
				write_index = i;
			}
			write_len += dsi->sw_sector;
		}
		else {
			_ND(trace_1_um_comm_process_zero_write_start_sep, "sw_lba=@SW_LBA no need for write - "
			   "check if we have pending writes", (u64)sw_lba);
			if (write_index > -1) {
				if (process_zero_write_start(pd, zreq, req,
					&req->reqs[write_index], write_lba, write_len)) {
					_NE(error_1_um_comm_process_zero_write_start_sep, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
					write_index = -1;
					break;
				}
				else {
					write_index = -1;
					write_lba = 0;
					write_len = 0;
				}
			}
		}
		pt += dsi->sw_sector;
		pm += dsi->sw_md_size;
	}
	if (write_index >= 0)
		if (process_zero_write_start(pd, zreq, req,
			&req->reqs[write_index], write_lba, write_len))
			_NE(error_2_um_comm_process_zero_write_start_sep, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
	NFOUT;
}

static void process_zero_write_start_no_md(struct per_disk *pd,
	struct zero_req *zreq, struct io_req *req, const char *str)
{
	struct disk_sector_info *dsi = zreq->dsi;
	int n_sw_sectors;
	void *p;
	unsigned long sw_lba;
	int i;
	bool is_zero;
	unsigned long write_lba = 0;
	int write_len = 0;
	int write_index = -1;

	NFIN;
	n_sw_sectors = req->reqs[0].data_len / dsi->sw_sector;
	p = req->vaddr;
	sw_lba = req->reqs[0].disk_block;
	for (i = 0; i < n_sw_sectors; ++i, ++sw_lba) {
		is_zero = is_buf_zero(p, dsi->sw_sector);
		if (zreq->is_test_zero) {
			if (!is_zero) {
				_NE(error_um_comm_process_zero_write_start_no_md, "Test zero failed on disk @DISK_ID_STR @ sw_lba=@SW_LBA",
					pd->di->disk_id, (u64)sw_lba);
				zreq->failed_sw_lba = sw_lba;
				zreq->stopped = true;
				break;
			}
		}
		else if (zreq->need_write(dsi, is_zero)) {
			_ND(trace_um_comm_process_zero_write_start_no_md, "sw_lba=@SW_LBA needs write", (u64)sw_lba);
			if (write_index < 0) {
				write_lba = sw_lba;
				write_len = 0;
				write_index = i;
			}
			write_len += dsi->sw_sector;
		}
		else {
			_ND(trace_1_um_comm_process_zero_write_start_no_md, "sw_lba=@SW_LBA no need for write - "
			   "check if we have pending writes", (u64)sw_lba);
			if (write_lba) {
				if (process_zero_write_start(pd, zreq, req,
					&req->reqs[write_index], write_lba, write_len)) {
					_NE(error_1_um_comm_process_zero_write_start_no_md, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
					write_index = -1;
					break;
				}
				else {
					write_index = -1;
					write_lba = 0;
					write_len = 0;
				}
			}
		}
		p += dsi->sw_sector;
	}
	if (write_index >= 0)
		if (process_zero_write_start(pd, zreq, req,
			&req->reqs[write_index], write_lba, write_len))
			_NE(error_2_um_comm_process_zero_write_start_no_md, "Fail to write @STR into disk @DISK_ID_STR", str, pd->di->disk_id);
	NFOUT;
}

static void handle_zero_test_start_write(struct per_disk *pd,
	struct zero_req *zreq, struct io_req *req, const char *str)
{
	NFIN;
	if (pd->di->mtdt_extd)
		process_zero_write_start_inline(pd, zreq, req, str);
	else if (pd->di->metadata)
		process_zero_write_start_sep(pd, zreq, req, str);
	else
		process_zero_write_start_no_md(pd, zreq, req, str);
	NFOUT;
}

static void handle_zero_read_end(struct per_disk *pd, struct zero_io_event *e)
{
	struct zero_req *zreq = e->zreq;
	struct disk_sector_info *dsi = zreq->dsi;
	struct io_req *io = e->io;
	const char *str = zreq == &dsi->zero ? "zero" : "data";
	int req_index = e - &io->es[0];
	struct nvmeibs_nvme_req *nvme_req = &io->reqs[req_index];

	NFIN;
	_ND(trace_um_comm_handle_zero_read_end, "End read on io[@REQ_INDEX]", (int)(io - &zreq->reqs[0]));
	io->write_in_progress = 0;
	if (!zreq->stopped) {
		if (e->status) {
			if (!zreq->is_test_zero) {
				if (e->status != READ_STATUS_NO_READ) {
					_NE(error_um_comm_handle_zero_read_end,
						"Failed to read, (status=@STATUS, result=@RESULT_INT), "
						"io[@REQ_INDEX], "
					   "io->reqs[@REQ_INDEX]. (disk_block=@DISK_BLOCK_LLONG, "
					   "data_len=@DATA_LEN_LONG), "
					   "so write the whole chunk @STR",
						e->status, e->result, (int)(e->io - &e->zreq->reqs[0]),
						req_index, (u64)nvme_req->disk_block,
						(unsigned long)nvme_req->data_len, str);
				}
				else {
					_NT(trace_1_um_comm_handle_zero_read_end, "@DISK_ID_STR: trim: skip read and start write",
						pd->di->disk_id);
					_ND(trace_2_um_comm_handle_zero_read_end, "Skip the read and go straight to write zero, "
					   "io[@REQ_INDEX], io->reqs[@REQ_INDEX]. (disk_block=@DISK_BLOCK, "
					   "data_len=@DATA_LEN_LONG), so write the whole chunk @STR",
						(int)(e->io - &e->zreq->reqs[0]), req_index,
						(unsigned long)nvme_req->disk_block, nvme_req->data_len, str);
				}
				process_zero_write_start(pd, zreq, io, &io->reqs[0],
					io->reqs[0].disk_block, io->reqs[0].data_len);
			}
			else {
				_NE(error_1_um_comm_handle_zero_read_end, "Test zero failed");
				zreq->failed_sw_lba = io->reqs[0].disk_block;
				zreq->stopped = true;
			}
		}
		else {
			handle_zero_test_start_write(pd, zreq, io, str);
			/* check whether we generated writes and
			   if no writes the read is done */
			if (!e->io->write_in_progress) {
				--zreq->read_in_progress;
				/* try to fetch more reads if we are not stopped -
				   stop can be called if write or test_zero failed
				*/
				if (!zreq->stopped)
					process_zero_read_start(pd, zreq, e->io);
			}
		}
	}
	else
		--zreq->read_in_progress;
	process_zero_check_zero_done(pd, zreq, str);
	NFOUT;
}

static int process_zero_read_start(
	struct per_disk *pd, struct zero_req *zreq, struct io_req *io)
{
	struct disk_sector_info *dsi = zreq->dsi;
	struct nvmeib_nl_uk_comm_msg *msg;
	struct nvmeib_zero_disk *zmsg;
	int n_sectors;
	int rv;

	NFIN;
	_ND(trace_um_comm_process_zero_read_start, "Start read on io[@IO_COUNT]", (int)(io - &zreq->reqs[0]));
		++zreq->read_in_progress;
		if (zreq->lba < zreq->last_lba_plus_1) {
			n_sectors = min((unsigned long)dsi->n_sw_sectors_bb * dsi->n_hw_in_sw,
				zreq->last_lba_plus_1 - zreq->lba);
			io->reqs[0].nvme_op = nvme_cmd_read;
			io->reqs[0].buf_offset = 0;
			io->reqs[0].disk_block = zreq->lba / dsi->n_hw_in_sw;
			io->reqs[0].data_len = n_sectors * dsi->hw_sector;
			io->reqs[0].buf_addrs = (dma_addr_t *)io->prpl_virt;
			io->reqs[0].prpl_phys = io->prpl_phys;
			zreq->lba += n_sectors;
			if (pd->di->mtdt_extd) {
				io->reqs[0].metadata = NULL;
				io->reqs[0].mtdt_size = 0;
				_ND(trace_1_um_comm_process_zero_read_start, "Inline MD: ...");
			}
			else if (pd->di->metadata) {
				io->reqs[0].metadata = io->md;
				io->reqs[0].mtdt_dma_ptr = io->md_dma;
				io->reqs[0].mtdt_size =
					(n_sectors / dsi->n_hw_in_sw) * dsi->sw_md_size;
				_ND(trace_2_um_comm_process_zero_read_start, "Sep MD: metadata=@METADATA, mtdt_size=@MTDT_SIZE_LONG",
					io->reqs[0].metadata, io->reqs[0].mtdt_size);
			}
			else {
				io->reqs[0].metadata = NULL;
				io->reqs[0].mtdt_size = 0;
				_ND(trace_3_um_comm_process_zero_read_start, "No MD: ...");
			}
			_ND(trace_4_um_comm_process_zero_read_start, "Reading: disk @DISK_ID_STR disk_block=@DISK_BLOCK, len=@LEN_LONG, n_hw_sectors=@N_HW_SECTORS_INT,"
			   "n_sw_sectors=@N_SW_SECTORS", pd->di->disk_id, (unsigned long)io->reqs[0].disk_block,
				io->reqs[0].data_len, n_sectors, n_sectors / dsi->n_hw_in_sw);
			io->reqs[0].cb = post_zero_read;
			io->reqs[0].arg = &io->es[0];
			io->es[0].e.type = umc_zero_read;
			msg = zreq->cur->p;
			zmsg = get_zero_disk(msg);
			if (zmsg->is_zeroing_using_test_and_write) {
				if ((rv = submit_local_cmd(pd->di, &io->reqs[0])) < 0) {
				_ND(trace_5_um_comm_process_zero_read_start, "Fail to submit cmd to disk @DISK_ID_STR (rv = @RV)",
					pd->di->disk_id, rv);
				zreq->stopped = true;
				goto end;
				}
			}
			else {
				_NT(trace_6_um_comm_process_zero_read_start, "@DISK_ID_STR: trim: skip read and call write", pd->di->disk_id);
				io->reqs[0].cb(io->reqs[0].arg, READ_STATUS_NO_READ, -1);
			}
			rv = 0;
		}
		else {
	end:
			--zreq->read_in_progress;
			rv = -1;
		}
		_ND(trace_7_um_comm_process_zero_read_start, "Disk @DISK_ID_STR: read_in_progress=@READ_IN_PROGRESS",
			pd->di->disk_id, zreq->read_in_progress);
		NFOUT;
		return rv;
	}

