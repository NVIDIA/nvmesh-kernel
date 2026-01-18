#ifndef NVMEIBC_IB_ADMIN_CHANNEL_H
#define NVMEIBC_IB_ADMIN_CHANNEL_H

#include "kr_incs.h"
#include "nvmeibc_admin_channel.h"
#include "nvmeibc_ib_net_admin.h"
#include "nvmeibc_types.h"
#include "nvmeibs_types.h"
#include "nvmeibc_stats.h"
#include "nvmeib_wd.h"
#include "nvmeib_types.h"
#include "nvmeib_public.h"
#include "nvmeibc_trend_types.h"

enum {
	/* the number of messages allocated for the admin channel - it includes
	   the io messages
	*/
	/* 128 - we need a lot of admin messages for resource shuffling among nodes,
	   this is the max number of GET-RSC msgs we may receive at once */
	NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_MSGS = 128,
	NVMEIBC_CHANNEL_MAX_MAIN_ADMIN_SG = 1,
};

struct nvmeibc_locks_channel;

struct nvmeibc_toma_recv_msg {
	u64 handle;

	/* reassemble  buffer of toma msg fragments */
	u8 *buf;
	/* current number of valid bytes witin @buf
	 *  != 0 - toma req is being assembled into buf
	 *  == 0 - buf is not in use
	 */
	int len;
	
	bool complete;
	
	struct volume_client_rsp *rsp;
	size_t rsp_sz;
	u64 rsp_dma_addr;
};

typedef void nvmeibc_disk_unsubscribe_toma_comp_callback_t(
	struct nvmeibc_disk *disk, int status, u64 handle);

typedef void nvmeibc_disk_async_subscribe_toma_comp_callback(
	struct nvmeibc_disk *disk, u64 handle, int status);

struct nvmeibc_ib_admin_channel_toma {
	//EC-5023:
	//Ths sole purpose of this flag is for ulp NOT send cmd (but instead defer
	//the cmd) after bad discover where ach is alive but we haven't created toma
	//yet i.e. prevent sending toma-cmd before ach control sequence had completed.
	bool valid;
	nvmeibc_disk_async_subscribe_toma_comp_callback *async_subscribe_comp_cb;
	nvmeibc_disk_unsubscribe_toma_comp_callback_t *unsubscribe_comp_cb;
	struct nvmeibc_toma_recv_msg recv_msg;
};

/* keep alive command work */
struct admin_ka_work {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	u64 ka_id;
};

struct nvmeibc_sync_admin_work {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	void (*cb)(int, void *);
	void *ctx;
};

struct nvmeibc_ib_admin_jmdc_read_req_work {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_disk_jmdc_read_comp *comp;
};

struct nvmeibc_ib_port;
struct nvmeib_rdma_ib_port_gid;
struct lock_seg_info;
/* the admin and no-RDDA channel */
struct nvmeibc_ib_admin_channel {
	/* our base channel */
	struct nvmeibc_admin_channel base;
	/* remote admin nic we are dealing with */
	char ranic_guid[GUID_SIZE];
	union ib_gid ranic_gid;
	/* the net for this channel */
	struct nvmeibc_ib_net_admin net;
	/* the transmit iu buffer */
	int tx_ring_size;
	struct nvmeib_iu **tx_ring;
	/* free transmit iu */
	struct list_head free_tx;
	/*uncompleted iu - handled after the channel is closed */
	struct list_head uncomp_tx;
	/* wait for send_msg complete*/
	struct completion send_done;
	/* well one always needs a lock... */
	spinlock_t guard;
	/* pending receive iu */
	struct list_head recv_ioctx;
	/* the iu context of the current send session */
	struct nvmeib_iu *send_ioctx;
	/* message area to send to controller */
	void *msg_area;
	void *msg_area_end;
	struct nvmeib_alloc_n_map msg_area_map;
	/* receive queue (if not running with SRQ) */
	struct nvmeib_recvq *recv_q;

	/* keep alive message buffer */
	spinlock_t ka_spinlock;
	struct nvmeib_hdr *ka_msg_area;
	u64 ka_msg_dma_addr;
	/* place holder for keep alive command */
	/* create it on demand */
	/* struct admin_ka_work ka_work; */
	/* indicates that keep alive message is underway */
	bool ka_sent;
	/* ka timer, if triggered, we raise keep alive time out */
	TIMER_LIST_INSTANCE(ka_timer);
	unsigned long ka_start_time;
	/*indicates that keep alive is running*/
	bool ka_running;

	/* controller message area info */
	u64 cmsg_buffer_raddr;
	u32 cmsg_buffer_pages;
	u32 cmsg_buffer_rkey;
	u32 cmsg_buffer_page_shift;

	/* piggyback read lock remote info */
	struct lock_seg_info *lsi;
	bool lsi_alloc;

	/* toma */
	struct nvmeibc_ib_admin_channel_toma toma;

	struct completion logout_comp;
	bool logout;
	enum nvmeibc_disk_release_reason release_reason;
};

/* controller command work */
struct admin_cmd_workq {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeib_iu *iu;
	struct completion *done;
	int rv;
};

/* controller command work */
struct admin_remove_workq {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
};

/* toma (disk) command work */
struct admin_toma_cmd_workq {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_disk_toma_cmd *toma_cmd;
	struct completion *done;
	int rv;
};

struct get_dirty_bits_info {
	struct nvmeibc_ib_admin_channel* ch;
	u32 seg_id;
	u64 start_addr;
	u64 len;
	struct nvmeibc_d_rdma_comp *comp;
	struct list_head link;
	struct workqe_struct work;
	int rv;
};

/* update local port work */
struct update_lport_workq {
	struct workqe_struct work;
	struct nvmeibc_ib_admin_channel *ch;
	struct nvmeibc_ib_port *port;
	struct completion *done;
	int rv_n;
};

struct volume_server_cmd_rgid_change_req;
struct admin_rgid_work {
	struct workqe_struct work;
	struct nvmeibc_disk *disk;
	struct nvmeibc_ib_admin_channel *ch;
	struct volume_server_cmd_rgid_change_req *r;
};

struct nvmeibc_please_kill_yourself_args {
	struct nvmeibc_ib_admin_channel *ch;
	bool has_death_wish;
	u64 rsc_id;
	u64 dlba;
};

static inline struct nvmeibc_ib_admin_channel *c_to_iac(
	struct nvmeibc_channel *ch)
{
	return container_of(c_to_ac(ch), struct nvmeibc_ib_admin_channel, base);
}

static inline struct nvmeibc_ib_admin_channel *ac_to_iac(
	struct nvmeibc_admin_channel *ch)
{
	return container_of(ch, struct nvmeibc_ib_admin_channel, base);
}

static inline struct nvmeibc_ib_admin_channel *ina_to_iac(
	struct nvmeibc_ib_net_admin *net)
{
	return container_of(net, struct nvmeibc_ib_admin_channel, net);
}

static inline struct nvmeibc_ib_admin_channel *in_to_iac(
	struct nvmeibc_ib_net *net)
{
	return ina_to_iac(in_to_ina(net));
}

static inline struct nvmeibc_ib_admin_channel *rionics_to_iac(
	struct list_head *rionics)
{
	return ac_to_iac(rionics_to_ac(rionics));
}

static inline struct nvmeibc_disk * nvmeibc_ib_admin_channel_disk(
	struct nvmeibc_ib_net *net)
{
	return in_to_iac(net)->base.base.disk;
}

struct nvmeibc_cinst_params_core;
struct nvmeibc_ib_admin_channel *nvmeibc_ib_admin_channel_create(
	const struct nvmeibc_cinst_params_core *p, struct nvmeibc_admin_rnic *arnic);
void nvmeibc_ib_admin_channel_free(struct nvmeibc_ib_admin_channel *ch);
int nvmeibc_ib_admin_channel_connect(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_disk *disk, struct nvmeibc_admin_rnic *arnic);
void nvmeibc_ib_admin_channel_disconnect(struct nvmeibc_ib_admin_channel *ch);
int nvmeibc_ib_admin_channel_access_iornics(struct nvmeibc_ib_admin_channel *ch,
	struct nvmeib_rdma_ib_port_gid *port_gids, u16 port_pkey, struct nvmeibc_ib_port *port);
int nvmeibc_ib_admin_channel_create_access_map(
	struct nvmeibc_ib_admin_channel *ch, bool is_rediscover);
int nvmeibc_ib_admin_channel_get_journal_range(
	struct nvmeibc_ib_admin_channel *ch);
int nvmeibc_ib_admin_channel_request_disks_resources(
	struct nvmeibc_ib_admin_channel *ch);
int nvmeibc_ib_admin_channel_connect_io_channel(
	struct nvmeibc_ib_admin_channel *ch, void *ioch);
int nvmeibc_ib_admin_channel_init_io_channel(
	struct nvmeibc_ib_admin_channel *ch, void *ioch,
	u64 disk_rsc_id, u64 msix_table_addr, u64 msix_address, u32 msix_payload);
int nvmeibc_ib_admin_channel_connect_nordda_channel(
	struct nvmeibc_ib_admin_channel *ch,
	struct nvmeibc_ib_nordda_channel *nrch);
struct nvmeibc_locks_channel *nvmeibc_ib_admin_channel_connect_lock_lb_channel(
	struct nvmeibc_ib_admin_channel *ch);
//bool nvmeibc_ib_admin_channel_has_disks(struct nvmeibc_ib_admin_channel *ch);
bool nvmeibc_ib_admin_channel_alive(struct nvmeibc_ib_admin_channel *ch);

void nvmeibc_ib_admin_channel_trace_path(struct nvmeibc_ib_admin_channel *ch);
void nvmeibc_ib_admin_channel_rm_locks_ch(struct nvmeibc_locks_channel *lch);
struct nvmeib_iu* nvmeibc_ib_admin_channel_get_tx_iu(
	struct nvmeibc_ib_admin_channel *ch);
void nvmeibc_ib_admin_channel_put_tx_iu(
	struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu);
struct nvmeib_iu* nvmeibc_ib_admin_channel_get_rx_iu(
	struct nvmeibc_ib_admin_channel *ch, int index);
int nvmeibc_ib_admin_channel_put_rx_iu(
	struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu);
int nvmeibc_ib_admin_channel_prepare_n_send_msg(
	struct nvmeibc_ib_admin_channel *ch, ssize_t (*f)(void *p, void *buf, const void *buf_end),
	void *p, int wr_opcode, struct nvmeib_iu *iu);
struct nvmeib_iu* nvmeibc_ib_admin_channel_pending_iu(
	struct nvmeibc_ib_admin_channel *ch, u64 req_tag);

int nvmeibc_ib_admin_send_toma_cmd(struct nvmeibc_ib_admin_channel *ch,
								   struct nvmeibc_disk_toma_cmd *toma_cmd);
int nvmeibc_ib_admin_send_toma_cmd_async(struct nvmeibc_ib_admin_channel *ch,
								   struct nvmeibc_disk_toma_cmd *toma_cmd);
void nvmeibc_ib_admin_recv_toma_cmd(struct nvmeibc_ib_admin_channel *ch);
bool nvmeibc_ib_admin_is_connected(struct nvmeibc_ib_admin_channel *ch);

void nvmeibc_ib_admin_channel_get_dirty_bits(struct workqe_struct *work_ptr);

void nvmeibc_ib_admin_channel_jmdc_read_work(struct workqe_struct *work);

void nvmeibc_ib_admin_channel_dbg_please_kill_yourself_work(struct workqe_struct *work);

void nvmeibc_ib_admin_channel_kill_remote_and_die(
    struct nvmeibc_please_kill_yourself_args *args);

void nvmeibc_ib_admin_channel_kill_remote_and_call(
    struct nvmeibc_please_kill_yourself_args *args,
	void (*cb)(void *), void *ctx);

struct volume_client_rsp;
int nvmeibc_ib_admin_send_rsp(struct nvmeibc_ib_admin_channel *ch,
			      u64 hdr_tag, struct volume_client_rsp *rsp,
			      int rsp_opcode, int wr_opcode, int rsp_len);
struct volume_server_req;
int nvmeibc_ib_admin_schedule_abnd2free(struct nvmeibc_disk *disk,
					struct volume_server_req *req);

#endif
