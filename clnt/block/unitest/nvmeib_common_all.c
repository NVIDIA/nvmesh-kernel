// For documentation, see Header in H file
/******************************************************************************/
#include "nvmeib_common_all.h"
#include "execinfo.h"


struct nvmeib_pcpu_wq *nvmeib_system_wq = NULL;
/**************************** common/nvmeib.h *********************************/
// Implementation of #include "nvmeib.h"
static int (*debug_level_f)(void);
void nvmeib_set_debug_level(int (*dlf)(void)) { debug_level_f = dlf; }
void nvmeib_register_local_client(struct nvmeib_local_client *c) { (void)c; };
int nvmeib_set_local_server_notification_calbacks(void (*cb)(struct nvmeib_local_server *s, void *arg), void *arg, void (*close_cb)(void *arg)){(void)cb; (void)arg; (void)close_cb; return 0;}
bool nvmeib_local_server_close_client(void){return true;}
bool nvmeib_local_client_close_server(void){return true;}

enum nvmeib_dev_type nvmeib_get_device_type(struct ib_device *ib_dev) { (void)ib_dev; return DT_uknown; }
bool nvmeib_is_dev_in_blacklist(struct ib_device *ib_dev) { (void)ib_dev; return false; }

struct nvmeib_dev *nvmeib_init(struct ib_device *device,
			       const char *inst_name,
			       bool do_init_cqs,
			       bool create_poll_cq_proc)
{
	struct nvmeib_dev     *res = (struct nvmeib_dev    *)kzalloc(sizeof(*res ),0);
	struct ib_device_attr *attr=( struct ib_device_attr*)kzalloc(sizeof(*attr),0);
	(void)do_init_cqs;
	(void)create_poll_cq_proc;
	(void)inst_name;
	memset(res,0,sizeof(*res));
	res->ib_dev 		= device;
	res->phys_port_cnt 	= 2;		// Simulate 2 ports infiniband and ethernet
	res->dev_attr = attr;
	attr->max_srq		= 117;
	return res;
}

void nvmeib_free(struct nvmeib_dev *dev){
	kfree(dev->dev_attr);
	kfree(dev);
}

int nvmeib_alloc_fast_reg_pool(struct nvmeib_dev *dev, struct ib_fmr_pool **fmr_pool, struct nvmeib_fr_pool **fr_pool, int pool_size, void *memmgr_metrics_ctx){
	(void)memmgr_metrics_ctx;
	(void)pool_size;
	if (dev->use_fast_reg) {
		*fr_pool = (struct nvmeib_fr_pool*)kzalloc(sizeof(struct nvmeib_fr_pool),0);
	} else {
		*fmr_pool = (struct ib_fmr_pool*)kzalloc(sizeof(struct ib_fmr_pool),0);
	}
	return 0;
}

void nvmeib_destroy_fast_reg_pool(struct nvmeib_fr_pool *pool, struct ib_mr **keep_mrs_arr, int *keep_mrs_arr_sz, void *memmgr_metrics_ctx, int pool_size){
	(void)keep_mrs_arr;
	(void)keep_mrs_arr_sz;
	(void)memmgr_metrics_ctx;
	(void)pool_size;
	kfree(pool);
}


int nvmeib_init_fast_reg(struct nvmeib_dev *dev){
	dev->use_fast_reg = 0;
	return 0;
}
void nvmeib_fast_reg_pool_trace(struct nvmeib_fr_pool *pool){ (void)pool; }

const char *nvmeib_device_name(struct nvmeib_dev *dev){return dev->ib_dev ? dev->ib_dev->name : "???";}

int nvmeib_create_cq_srq(struct nvmeib_dev *dev, int msg_size, void *memmgr_metrics_ctx) { (void)dev; (void)msg_size; (void)memmgr_metrics_ctx; return 0; }
int nvmeib_dev_cq_stat_hdr(char *buffer, size_t len) { (void)buffer; (void)len; return 0; }
int nvmeib_dev_cq_stat(struct nvmeib_dev *dev, char *buffer, size_t len) { (void)dev; (void)buffer; (void)len; return 0; }
int nvmeib_dev_cq_stat_reset(struct nvmeib_dev *dev) { (void)dev; return 0; }

bool nvmeib_support_srq(struct nvmeib_dev *dev) { (void)dev; return false; }

// create volume shared receive queue
#include "nvmeib_srq.h"
int nvmeib_srq_pool_create(struct nvmeib_dev *dev, int pool_size, struct nvmeib_srq_params *prim_q_params, struct nvmeib_srq_params *sec_qs_params, void *memmgr_metrics_ctx){
	(void)dev; (void)pool_size; (void)prim_q_params; (void)sec_qs_params; (void)memmgr_metrics_ctx; return 0;
}

static struct nvmeib_intr_shaper intr_shaper;

struct nvmeib_intr_shaper *nvmeib_intr_shaper_create(u64 frame_size_usecs){
	(void)frame_size_usecs;
	return &intr_shaper;
}
struct nvmeib_intr_shaper *nvmeib_get_intr_shaper(void){ return &intr_shaper; }
void nvmeib_intr_shaper_destroy(struct nvmeib_intr_shaper *shaper){ BUG_ON(shaper != &intr_shaper); }
void nvmeib_dump_buf(const void *buf, int len){ (void)buf; (void)len; BUG(); }

/*************************** common/nvmeib_wd.h *******************************/
// Implementation of #include "nvmeib_wd.h"
struct wd_obj {
	int timeout_sec;
	int n_buckets;
	int hz_per_bucket;
};
struct wd_obj* nvmeib_wd_create_on_cpu(unsigned int timeout_sec, unsigned int n_buckets, unsigned int hz_per_bucket, int cpu){
	struct wd_obj *wd = kzalloc(sizeof(*wd), GFP_ATOMIC);
	(void)cpu;
	wd->timeout_sec = timeout_sec; wd->n_buckets = n_buckets; wd->hz_per_bucket = hz_per_bucket;
	return wd;
}
void nvmeib_wd_remove(   struct wd_obj *wd){ BUG_ON(!wd); kfree(wd); wd = NULL; }
void nvmeib_wd_add_entry(struct wd_obj *wd, struct nvmeib_wd_entry *e){ (void)wd; (void)e; }
void nvmeib_wd_del_entry(struct wd_obj *wd, struct nvmeib_wd_entry *e){ (void)wd; (void)e; }

/************************** common/nvmeib_rdma.h ******************************/
// Implementation of #include "nvmeib_rdma.h"
int nvmeib_rdma_fill_gids(struct ib_device *ib, int port, struct ib_port_attr *port_attr, union ib_gid *gid, union ib_gid *hw_gid, union ib_gid *vlan_gids, int nvlans, enum rdma_link_layer *layer){
	(void)port_attr;
	(void)ib; (void)gid; (void)hw_gid; (void)vlan_gids; (void)nvlans;
	*layer = (port%2) ? IB_LINK_LAYER_INFINIBAND : IB_LINK_LAYER_ETHERNET;	// Simulate alternation between ports
	return 0;
}

int	   nvmeib_rdma_select_port_gid(struct ib_device *ib, int port, struct ib_port_attr *port_attr, int start_gid_index, struct nvmeib_rdma_ib_port_gid *rdma_port_gid, bool allow_default_gid, bool allow_ipv6_link_local, const char *match_ndev_name, const union ib_gid *match_hw_gid, const union ib_gid *match_sw_gid_value, const union ib_gid *match_sw_gid_mask) { (void)ib; (void)port; (void)port_attr; (void)start_gid_index; (void)rdma_port_gid; (void)allow_default_gid; (void)allow_ipv6_link_local; (void)match_ndev_name; (void)match_hw_gid; (void)match_sw_gid_value; (void)match_sw_gid_mask; return 0; }

char * nvmeib_rdma_gid_type_str(enum nvmeib_rdma_gid_type gid_type, enum rdma_link_layer link_layer){ (void)gid_type; (void)link_layer; return NULL;}
int    nvmeib_rdma_port_supports_gid_type(struct ib_device *ib, int port, enum nvmeib_rdma_gid_type gid_type){ (void)ib; (void)port; (void)gid_type; return 0;}
struct nvmeib_rdma_cm* nvmeib_rdma_listen(struct nvmeib_rdma_listen_params *params){ (void)params; return NULL;}
void   nvmeib_rdma_destroy_cm(struct nvmeib_rdma_cm *cm){ (void)cm; }

struct event_handler_private { struct nvmeib_rdma_event_handler event_handler; };
int nvmeib_rdma_register_event_handler(struct ib_device *ib_dev, nvmeib_rdma_event_handler_fn handler_fn, void *ctx, struct nvmeib_rdma_event_handler **handler){
	struct event_handler_private *handler_private;
	BUG_ON(!ib_dev || !handler_fn || !handler);
	BUG_ON(!(handler_private = kzalloc(sizeof(struct event_handler_private) /*+ (ib_dev->phys_port_cnt * sizeof(struct event_handler_port_data))*/, GFP_KERNEL)));
	handler_private->event_handler.ib_dev     = ib_dev;
	handler_private->event_handler.handler_fn = handler_fn;
	handler_private->event_handler.ctx        = ctx;
	*handler = &handler_private->event_handler;
	return 0;
}

int nvmeib_rdma_unregister_event_handler(struct nvmeib_rdma_event_handler *handler){
	struct event_handler_private *handler_private = container_of(handler, struct event_handler_private, event_handler);
	kfree(handler_private);
	return 0;
}

/************************** common/nvmeib_ib_driver.h *******************************/
bool nvmeib_device_sup_cap(enum nvmeib_dev_type t, enum nvmeib_dev_cap cap) { (void)t; (void)cap; return true; }

/************************** clnt/nvmeibc_ib_net.h *****************************/
// Implementation of #include "nvmeibc_ib_net.h"
static int *intershaper = NULL;
int  nvmeibc_ib_net_intr_shaper_create(void){
	BUG_ON(intershaper);
	intershaper = kzalloc(sizeof(int),0);
	return 0;
}
void nvmeibc_ib_net_intr_shaper_destroy(void){
	BUG_ON(!intershaper);
	kfree(intershaper);
	intershaper = NULL;
}

/********************* common_public/nvmeib_public.h **************************/
// Implementation of #include "nvmeib_public.h"
int nvmeib_public_cache_line_size(void) { return sizeof(unsigned long); }

int nvmeib_public_debug_level(void) {
	return nvmeib_debug_level(); // Daniel: use the same param of nvmeibc for nvmeib
}

void nvmeib_public_set_debug_level(int (*dlf)(void)) { debug_level_f = dlf; }

int nvmeib_public_copy_user_pages(void *kbuf, int pid, void *ubuf, int len, int copy_to) {
	(void)pid;
	if (copy_to)
		memcpy(ubuf, kbuf, len);
	else
		memcpy(kbuf, ubuf, len);
	return 0;
}

int nvmeib_public_user_pages_for_io_pin(pid_t pid, ulong userspace_vaddr, int n_pages, struct page **pages, int is_write)
{
	(void)pid;
	return get_user_pages_fast(userspace_vaddr, n_pages, is_write, pages);
}

void nvmeib_public_user_pages_for_io_unpin(int n_pages, struct page **pages, int copy_to) {
	int i;   (void)copy_to;
	for (i = 0; i < n_pages; i++)
		put_page(pages[i]);
}

void __percpu *__nvmeib_public_alloc_percpu(size_t size, size_t align){ return __alloc_percpu(size, align); }
void __percpu *__nvmeib_public_alloc_percpu_zeroed(size_t size, size_t align) { return __alloc_percpu(size, align); }
void nvmeib_public_free_percpu(void __percpu *ptr) { return free_percpu(ptr); }
bool nvmeib_public_serial_console(void){ return false; }

/* Common EC functions to client and server filled as function pointer*/
static struct {
	void (*read_mod_wr)(void*, u64);
} block_ec_dp_funcs = {NULL};
void nvmeib_block_dp_ec_dmd_read_mod_wr(void *dmd_ptr, u64 param){ block_ec_dp_funcs.read_mod_wr(dmd_ptr, param); }
void nvmeib_set_block_dp_ec_funcs(void (*read_mod_wr)(void*, u64)){ block_ec_dp_funcs.read_mod_wr = read_mod_wr;}

/************************** nvme_public module Simulator **********************/
static struct nvmeib_public_kth_ft *kth_ft;
const struct nvmeib_public_kth_ft * nvmeib_kth_ft(void) { return kth_ft;}
void nvmeib_kth_set_ft(struct nvmeib_public_kth_ft *ft) {
	kth_ft = kzalloc(sizeof(*kth_ft), GFP_KERNEL);
	*kth_ft = *ft;
}

void nvmeib_kth_unset_ft(void) {
	kfree(kth_ft);
	kth_ft= NULL;
}

// initialize simulation of nvmeib_public module
void nvmeib_public_init(void) {
	struct nvmeib_public_kth_ft ft = {
		.f_current = nvmeib_public_kth_current,
		.f_get_name = nvmeib_public_kth_get_name,
		.f_e2s = nvmeib_public_kth_e2s,
		.f_add_event = nvmeib_public_kth_add_event,
		.f_wait_events = nvmeib_public_kth_wait_events,
		.f_wait_events_timeout = nvmeib_public_kth_wait_events_timeout
	};
	nvmeib_public_kth_init_lib();
	nvmeib_kth_set_ft(&ft);
}

void nvmeib_public_module_exit(void) { /* Destructor */
	nvmeib_kth_unset_ft();
}
DEFINE_COMMON_REGISTER_KEEPER_FN(nvmeib_register_keeper) { (void)ops; return -EOPNOTSUPP; }
DEFINE_COMMON_UNREGISTER_KEEPER_FN(nvmeib_unregister_keeper) { (void)ops; }

struct nvmeib_keeper_ops *nvmeib_public_get_keeper(void) { return NULL; }
void nvmeib_public_put_keeper(void) {}

int nvmeib_public_load_keeper(void) { return -EOPNOTSUPP; }
int nvmeib_public_unload_keeper(void) { return -EOPNOTSUPP; }

void nvmeib_public_keeper_init(void) {}
void nvmeib_public_keeper_fini(void) {}

/*********************** clnt/nvmeibc_ib_admin_channel.h **********************/
// Implementation of #include "nvmeibc_ib_admin_channel.h". Needed for Toma receive methods
struct nvmeib_iu* nvmeibc_ib_admin_channel_get_tx_iu( struct nvmeibc_ib_admin_channel *ch){ (void)ch; BUG_NOT_IMPLEMENTED_YET; return NULL; }
struct nvmeib_iu* nvmeibc_ib_admin_channel_pending_iu(struct nvmeibc_ib_admin_channel *ch, u64 req_tag){ (void)ch; (void)req_tag; BUG_NOT_IMPLEMENTED_YET; return NULL; }
void 			  nvmeibc_ib_admin_channel_put_tx_iu( struct nvmeibc_ib_admin_channel *ch, struct nvmeib_iu *iu){ (void)ch; (void)iu; BUG_NOT_IMPLEMENTED_YET; }
void 			  nvmeibc_ib_admin_recv_toma_cmd(	  struct nvmeibc_ib_admin_channel *ch){ (void)ch; BUG_NOT_IMPLEMENTED_YET; }

int nvmeibc_ib_admin_channel_prepare_n_send_msg(
	struct nvmeibc_ib_admin_channel *ch, ssize_t (*f)(void *p, void *buf, const void *buf_end),
	void *p, int wr_opcode, struct nvmeib_iu *iu) { (void)ch; (void)f; (void)p; (void)wr_opcode; (void)iu; BUG_NOT_IMPLEMENTED_YET; return 0;}
int 			  nvmeib_post_recv(struct nvmeib_dev *dev, struct nvmeib_iu *iu){ (void)dev; (void)iu; BUG_NOT_IMPLEMENTED_YET; return 0; }

/************************** common/nvmeib_utils.h ******************************/
// Implementation of #include "nvmeib_utils.h". Todo: Remove this code and extend unitest to be on top of nvmeibc_utils.c
void format_gid_raw(u8 raw[16], char *buf){
	int i, n;
	for (n = 0, i = 0; i < NVMEIB_NUM_OF_GID_SECTIONS; ++i) {
		n += sprintf(buf + n, NVMEIB_GID_SECTION_FORMAT, be16_to_cpu(((__be16 *)raw)[i]));
		if (i < (NVMEIB_NUM_OF_GID_SECTIONS -1)) buf[n++] = ':';
	}
	BUG_ON(n>=GUID_SIZE);
}

void format_gid(union ib_gid *gid, char *buf){format_gid_raw(gid->raw, buf);}
bool nvmeib_use_dev(struct list_head *used_dev_list, struct ib_device *device, u8 port, struct list_head *out_list) { (void)used_dev_list; (void)device; (void)port; (void)out_list; return true;}
int nvmeib_set_used_dev_list(  const char *param, unsigned max_len, struct list_head *used_dev_list){ (void)param; (void)max_len; (void)used_dev_list; return 0; }
int nvmeib_set_used_pots_guids(const char *param, unsigned max_len, struct list_head *used_dev_list){ (void)param; (void)max_len; (void)used_dev_list; return 0; }
void nvmeib_free_used_dev_list(struct list_head *used_dev_list){ (void)used_dev_list;
	return;	// Todo: clear this list
}

bool nvmeib_dev_use_keeper(struct nvmeib_dev *dev) {
	(void)dev;
	return false;
}

int nvmeib_run_usermode_script(const char *script_name) {
	int rv = 0;
	if (!script_name || script_name[0] == '\0')
		return -1;
	BUG_NOT_IMPLEMENTED_YET;
	//rv = call_usermodehelper(argv[0], argv, envp, wait ? UMH_WAIT_PROC : UMH_NO_WAIT);
	return rv;
}
int nvmeib_set_roce_lossy_mode_on(void) {return 0;}
#include "common/compat/kr_incs_str.inc.c"

static atomic64_t global_uid = ATOMIC_INIT(0);
u64 nvmeib_get_guid(void) {return atomic64_inc_return(&global_uid);}

/********************* common/nvmeib_msgloop.c Simulator **********************/
#include "nvmeib_msgloop.h"
struct msgloop_procfs_ent {
	struct list_head link;								// Link of all msgloops to inject read arguments
	void *write_arg;
	int (*write_cb)(void *arg, char *buf, size_t len, bool *posted);	// client receives msg from simulator
	void (*on_close_cb_unused)(void *);									// Callback of client when message loop is opened or closed
	void (*on_open_cb_unused)( void *);
	struct proc_dir_entry *dir;
	//struct proc_dir_entry *dir; struct proc_dir_entry *ent; bool shutdown; bool opened;
	char *name;
	int msg_count, max_msg;												// Daniel: Todo, simualte this
	// For simulators api use
	void *read_arg;
	int (*read_cb)(void *arg, const char *buf, size_t len);	// Simulator receives msg from client
};
static LIST_HEAD(all_msgloops_procfs_list);

static ssize_t msgloop_proc_read(struct file *file, char __user *buf, size_t len, __attribute__ ((unused)) loff_t *offset_p) {
	struct msgloop_procfs_ent *p = PDE_DATA(file_inode(file));
	return (ssize_t)p->read_cb(p->read_arg, (char*)buf, len);		  //Simulator (userspace) reads  data written by kernel client (live code)
}

static ssize_t msgloop_proc_write(struct file *file, const char __user *buf, size_t len, __attribute__ ((unused)) loff_t *offset_p) {
	struct msgloop_procfs_ent *p = PDE_DATA(file_inode(file));
	return (ssize_t)p->write_cb(p->write_arg, (char*)buf, len, NULL); //Simulator (userspace) writes data        for kernel client (live code)
}

static int msgloop_proc_release(struct inode *i, struct file *file) {
	struct msgloop_procfs_ent *p = PDE_DATA(file_inode(file)); (void)i;
	p->on_close_cb_unused(p->write_arg); // Simulator (userspace) closes after verification of expected output
	return 0;
}

static int msgloop_proc_open(struct inode *i, struct file *file) {
	struct msgloop_procfs_ent *p = PDE_DATA(file_inode(file)); (void)i;
	p->on_open_cb_unused(p->write_arg); // Simulator (userspace) opens after setting verification expectors
	return 0;
}

static const struct file_operations msgloop_fops = {.read = msgloop_proc_read, .write = msgloop_proc_write, .open = msgloop_proc_open, .release = msgloop_proc_release};

// we curretnly don't support flushing in simulation
void nvmeib_msgloop_set_ready_cb(struct msgloop_procfs_ent *p,
				 void (*ready_cb)(void *))
{
	(void)p;
	(void)(ready_cb);
}

struct msgloop_procfs_ent *nvmeib_msgloop_create(char *name,
	struct proc_dir_entry *dir, int (*cb)(void *, char *, size_t, bool *posted),
	void (*on_open_cb)(void *), void (*on_close_cb)(void *), void *arg){
	struct msgloop_procfs_ent *p = kzalloc(sizeof(*p), GFP_KERNEL);
	p->write_cb    = cb;
	p->on_close_cb_unused = on_close_cb;
	p->on_open_cb_unused  = on_open_cb;
	p->write_arg   = arg;
	p->name = kstrdup(name, GFP_KERNEL);
	dir->fops = &msgloop_fops;					// Daniel: Should create proc entry with fops, To save code we put fops on directory itself knowing it contains only 1 file
	p->dir = dir;
	list_add(&p->link, &all_msgloops_procfs_list);
	return p;
}

struct msgloop_msg *nvmeib_msgloop_alloc_msg(size_t data_size)
{
	struct msgloop_msg *msg;

	msg = kzalloc(sizeof(*msg) + data_size, GFP_KERNEL);
	if (msg) {
		kref_init(&msg->ref_cnt);
	}

	return msg;
}

static void msgloop_free_msg(struct kref *kref)
{
       struct msgloop_msg *msg = container_of(kref, struct msgloop_msg, ref_cnt);
       kfree(msg);
}

void msgloop_get_msg(void *_msg)
{
	struct msgloop_msg *msg = _msg;
	if (msg)
		kref_get(&msg->ref_cnt);

}

void msgloop_put_msg(void *_msg)
{
	struct msgloop_msg *msg = _msg;
	if (msg)
		kref_put(&msg->ref_cnt, msgloop_free_msg);
}

void nvmeib_msgloop_remove(struct msgloop_procfs_ent *p){
	if (p == NULL)
		return;
	p->dir->fops = NULL;					// Daniel: Should create proc entry with fops, To save code we put fops on directory itself knowing it contains only 1 file
	list_del(&p->link);
	kfree(p->name);
	kfree(p);
}

size_t nvmeib_msgloop_flush(struct msgloop_procfs_ent *p){ (void)p; return 0; }

int nvmeib_msgloop_get_count(const struct msgloop_procfs_ent *p) {
	return (p != NULL) ? p->msg_count : 0;
}

/* Take the message out of the msgloop object and call a handle function */
int nvmeib_msgloop_sendl(struct msgloop_procfs_ent *p, struct list_head *l){
	struct msgloop_msg *msg;
	while (!list_empty(l)) {
		msg = list_first_entry(l, struct msgloop_msg, link);
		if (p->read_cb) {
			(void)msgloop_proc_read((void*)p, msg->data, msg->len, NULL);
		} else {
			cpu_relax(); // user space app is not ready to read from msgloop. Drop it.
			// Todo: Fix simulators and add assert as this should not happen! Kernel should never write to message loop without user app listening!
		}
		list_del(&msg->link);
		msgloop_put_msg(msg);
	}
	return 0;
}

void nvmeib_msgloop_set_max(struct msgloop_procfs_ent *p, int max_msg) {
	p->max_msg = max_msg;
}

/*  This function gets all strings sent to cli and will enforce expctations - this will centralize the expected CLI replies
    and allow blk_unitest run to wait until all async calls are done (volume reached expcted status) */
int nvmeib_msgloop_send(struct msgloop_procfs_ent *p, char *data, size_t len){
	if (p->read_cb) {
		return (int)msgloop_proc_read((void*)p, data, len, NULL);
	} else {
		return -EPIPE;
	}
}

void* msgloop_proc_inject_reader_cb(const char *proc_dir_path, const char* msgloop_name, void* arg,
                                    int (*cb)(void* arg, const char* buf, size_t len)) {
	struct msgloop_procfs_ent *ml;
	list_for_each_entry(ml, &all_msgloops_procfs_list, link) {
		if ((strcmp(ml->name, msgloop_name))||(strcmp(ml->dir->parent->name, proc_dir_path)))
			continue;	// Wrong file or wrong client instance
		ml->read_arg = arg;
		ml->read_cb  = cb;
		return ml->write_arg;
	}
	BUG();					// Bug in simulator
	return NULL;
}

#include "common/compat/kr_incs_nvmeib_common.inc.c"

void nvmeib_public_save_stack_trace(struct stack_trace *trace)
{
	backtrace((void**)trace->entries, trace->max_entries);
}

void nvmeib_ref_init(struct nvmeib_ref *r)
{
	atomic_set(&r->cnt, 1);
	r->comp = NULL;
	atomic_set(&r->dying, 0);
}

int __must_check nvmeib_ref_get(struct nvmeib_ref *r)
{
	int _old;
	if (!atomic_read(&r->dying))
		_old = atomic_inc_not_zero(&r->cnt);
	else
		_old = 0;
	return _old;
}

int nvmeib_ref_put(struct nvmeib_ref *r)
{
	int _new;
	if (!(_new = atomic_dec_return(&r->cnt))) {
		if (r->comp)
			complete(r->comp);
		else WARN_ON(1);
	}
	return _new;
}

int nvmeib_ref_release_start(struct nvmeib_ref *r)
{
	int dying, rv;
	if ((dying = atomic_inc_return(&r->dying)) == 1) {
		_NT(nvmeib_ref_release_start_t1, "r @PTR: initiate release", r);
		rv = 0;
	} else {
		_NT(nvmeib_ref_release_start_t2,
			"r @PTR: release already initaited (@INT)", r, dying);
		rv = -1;
	}
	return rv;
}

void nvmeib_ref_release_wait_n(struct nvmeib_ref *r, unsigned num_attempts)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	int n, rvw;
	unsigned a = 0;

	/* release prior init */
	if (atomic_read(&r->cnt) <= 0)
		goto out;

	r->comp = &comp;
	if ((n = atomic_dec_return(&r->cnt))) {
		_NT(nvmeib_ref_release_wait_t1, "r @PTR: wait for @INT to finish",
			r, n);
		while ((rvw = wait_for_completion_interruptible_timeout(
			r->comp, NVMEIB_REF_WAIT_RELEASE)) <= 0) {
			_NW(nvmeib_ref_release_wait_w1,
				"r @PTR: wait for @INT to finish, attempt @INT",
				r, atomic_read(&r->cnt), a);
			a++;
			if (a == num_attempts) {
				_NW(nvmeib_ref_release_wait_w2,
					"r @PTR: max attempts @INT. Giving up",
					r, num_attempts);
				break;
			}
		}
	}
	r->comp = NULL;

out:
	return;
}

unsigned int nvmeib_get_tcp_base_port_id(void) {
	return NVMEIB_EXCELERO_IWARP_PORT_ID;
}

int nvmeib_public_kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp_ext[])
{
	(void) kobj;
	(void) action;
	(void) envp_ext;
	return 0;
}

int nvmeib_buffer_alloc_sgl_from_pages(struct nvmeib_buffer *buf, struct page **pages,
				       unsigned int n_pages, unsigned int size, unsigned int offset, gfp_t gfp_mask)
{
	int rv = sg_alloc_table(&buf->sgt, n_pages, gfp_mask);
	unsigned i;
	struct scatterlist *sg;

	if (rv < 0)
		goto out;

	for_each_sg(buf->sgt.sgl, sg, n_pages, i) {
		sg_set_page(sg, pages[i], PAGE_SIZE, 0);
	}

	buf->size = size;
	buf->offset = offset;
out:
	return rv;
}

/*****************************************************************************/
// EOF.
