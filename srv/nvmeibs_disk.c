#include "kr_incs.h"
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/syscalls.h>
#include <linux/inet.h>

#include "nvmeibs_defs.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_disk.h"
#include "nvmeibs_client.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_toma.h"
#include "nvmeibs_um_comm.h"
#include "nvmeibs_trace.h"
#include "nvmeib_io_stats.h"
#include "common/proc_epilog.h"
//#define CONFIG_NVMEIB_DEBUG
//#define DEBUG
#include "nvmeib_utils.h"

bool nvmeibs_disk_collect_stats = true;
module_param_named(disk_collect_stats, nvmeibs_disk_collect_stats, bool, 0644);
MODULE_PARM_DESC(disk_collect_stats, "Enable collecting statistics for disk operations. Can be used for performance optimization.");


static LIST_HEAD(disk_info_list);
static int disk_count;
static LIST_HEAD(disk_rm_list);

static DEFINE_MUTEX(guard);
static atomic_t n_mmaps = ATOMIC_INIT(0);

struct client_info_;
struct rsc_info_ {
	struct nvmeibs_q_info *qs;
	struct client_info_ *ci;
	struct list_head link;
};

struct disk_ {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u16 vendor_id;
	/* nvme-drive info */
	struct nvmeibs_disk_info *info;
	struct rsc_info_ *rscs;
	/* the free disk resources */
	int n_a; //16
	struct list_head avail_rsc;

	/* the number of clients with resources
	   e.g. for a single client case, this
	   value will be 1 although it uses all
	   resources */
	int n_wi;
	/* the clients with list */
	struct rb_root clients_with;
	/* the number of client waiting for resources */
	int n_wa;
	/* the client waiting list */
	struct list_head clients_wait;
	/* buffer for send client resources */
	__be64 ids[NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT];
	/* contains the disk segments lock information */
	struct nvmeibs_disk_private_data locks_priv;
	/* on locks-proc remove done actions */
	bool free_locks_mem;
	/* if !empty, disk is being removed */
	struct list_head rm_link;
	/* new nvme-drive info add request deferred
	   until this disk obj removal completes */
	struct nvmeibs_disk_info *new_info;
	/* list of prefered ports */
	struct list_head prefered_ports;
};

struct client_info_ {
	struct nvmeibs_client *cl;
	struct nvmeibs_client_disk *cdisk;
	void *locate_buf_start; // TODO remove
	void *locate_buf_end;	// TODO remove
	__be64 *locate_buf_p;	// TODO remove
	struct disk_ *disk;
	/* the current number of resources the client holds */
	int n_rsc;
	/* the number of resources the client was asked to return */
	int r_rsc;
	/* the current number of resources the client is waiting for */
	int w_rsc;
	/* the number of requests already issued to client */
	int t_rsc;
	/* the list of the client resources */
	struct list_head rsc;
	struct rb_node node;
	struct list_head link;
};

struct list_head* nvmeibs_disk_get_disks(int *size)
{
	static int warned = 0;

	/* catch flows locking devices before disks */
	if (!warned) {
		if (nvmeibs_is_devices_locked_by_me()) {
			_NT(trace_disk_nvmeibs_disk_get_disks, "Attempt locking disks while devs locked");
			//dump_stack();
			warned = 1;
		}
	}

	mutex_lock(&guard);
	if (size)
		*size = disk_count;
	return &disk_info_list;
}

struct list_head* nvmeibs_disk_get_disks_nolock(int *size)
{
	if (size)
		*size = disk_count;
	return &disk_info_list;
}

void nvmeibs_disk_put_disks(void)
{
	mutex_unlock(&guard);
}

int nvmeibs_disk_add_preferd_port(struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port *port)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);
	struct nvmeibs_disk_prefered_port *p;
	int rv = 0;

	NFIN;

	if (nvmeibs_disk_is_prefered_port(di, port)) {
		_NT(trace_disk_nvmeibs_disk_add_preferd_port, "Port @PORT_PTR is alread set in previous rule", port);
		goto out;
	}

	if (!(p = kzalloc(sizeof(*p), GFP_KERNEL))) {
		_NE(error_disk_nvmeibs_disk_add_preferd_port, "mem alloc problem");
		WARN_ON(true);
		rv = -ENOMEM;
		goto out;
	}
	p->prefered_port = port;
	list_add(&p->link, &disk->prefered_ports);
	_NT(trace_1_disk_nvmeibs_disk_add_preferd_port, "disk=@DISK prefports=@PREFPORTS p=@PREF_PORT", disk, &disk->prefered_ports, p);
out:
	NFOUT;
	return rv;
}

/* remove port from disk prefered list */
void nvmeibs_disk_remove_prefered_port(struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port *port)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);
	struct nvmeibs_disk_prefered_port *p;

	NFIN;
	list_for_each_entry(p, &disk->prefered_ports, link) {
		if (p->prefered_port == port) {
			list_del(&p->link);
			kfree(p);
			break;
		}
	}

	NFOUT;
}

bool nvmeibs_disk_is_prefered_port(struct nvmeibs_disk_info *di,
	struct nvmeibs_ib_port *port)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);
	struct nvmeibs_disk_prefered_port *p;
	bool rv = false;

	NFIN;
	if (!disk)
		_NT(trace_disk_nvmeibs_disk_is_prefered_port, "di=@DI, di->priv=@PRIV, disk=@DISK", di, di->priv, disk);
	else
		list_for_each_entry(p, &disk->prefered_ports, link) {
	if (p->prefered_port == port) {
		rv = true;
		break;
	}
}

NFOUT;
return rv;
}

/* retrun a list of prefered ports*/
struct list_head* nvmeibs_disk_prefered_ports(struct nvmeibs_disk_info *di)
{
	struct nvmeibs_disk_private_data *disk_pd = di->priv;
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);

	NFIN;
	_NT(trace_disk_nvmeibs_disk_prefered_ports, "disk=@DISK prefports=@PREFPORTS p=@NEXT", disk, &disk->prefered_ports, disk->prefered_ports.next);

	NFOUT;
	return &disk->prefered_ports;
}

static struct client_info_* ci_rb_insert(struct rb_root *root,
	struct client_info_ *ci, int allow_duplicates)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	struct client_info_ *cur_ci;
	int rv;

	NFIN;
	while (*link) {
		parent = *link;
		cur_ci = rb_entry(parent, struct client_info_, node);

		rv = (cur_ci->n_rsc - cur_ci->r_rsc) - (ci->n_rsc - ci->r_rsc);
		if (rv < 0)
			link = &(*link)->rb_left;
		else if (rv > 0)
			link = &(*link)->rb_right;
		else if (allow_duplicates)
			link = &(*link)->rb_left;
		else
			goto out;
	}
	rb_link_node(&ci->node, parent, link);
	rb_insert_color(&ci->node, root);
	cur_ci = NULL;

out:
	NFOUT;
	return cur_ci;
}

static struct client_info_* ci_insert(struct disk_ *disk,
	struct client_info_ *ci, int allow_duplicates)
{
	return ci_rb_insert(&disk->clients_with, ci, allow_duplicates);
}

static struct client_info_* create_client(
	struct nvmeibs_client *cl)
{
	struct client_info_ *ci = kzalloc(sizeof(*ci), GFP_KERNEL);
	int rv = nvmeibs_client_create_disk_cache(cl, &disk_info_list, disk_count);
	_ND(trace_disk_create_client, "disk_count=@DISK_COUNT", disk_count);

	NFIN;
	if (!rv && ci) {
		ci->cl = cl;
		INIT_LIST_HEAD(&ci->rsc);
		INIT_LIST_HEAD(&ci->link);
	} else {
		if (ci) {
			kfree(ci);
			ci = NULL;
		}
		_NE(error_disk_create_client, "OOM: fail to allocate client info");
	}
	NFOUT;
	return ci;
}

static void free_disk(struct disk_ *disk)
{
	struct nvmeibs_disk_prefered_port *p;
	_NT(trace_disk_free_disk, "free disk @DISK, disk-pd @LOCKS_PRIV", disk, &disk->locks_priv);
	BUG_ON(disk->locks_priv.proc_locks);
	BUG_ON(disk->locks_priv.n_memsegs);
	while ((p = list_first_entry_or_null(&disk->prefered_ports,
				struct nvmeibs_disk_prefered_port, link))) {
		list_del(&p->link);
		kfree(p);
	}
	kfree(disk->rscs);
	kfree(disk);
}

void locks_proc_on_last_mmap(void *arg);

struct __fill_disk_client_status_ctx {
	char *buf;
	size_t len;
	ssize_t *count;
	int *n_clients;
	struct nvmeibs_disk_info *di;
	const char *indent;
};

static int __fill_disk_client_status(struct nvmeibs_client *cl, void *arg) {
	struct __fill_disk_client_status_ctx *ctx = arg;
	if (cl->di == ctx->di) {
		++*ctx->n_clients;
		*ctx->count += scnprintf(ctx->buf + *ctx->count, ctx->len - *ctx->count, "%s- name: %s\n",
			ctx->indent, cl->name);
		*ctx->count += scnprintf(ctx->buf + *ctx->count, ctx->len - *ctx->count, "%s  ptr: 0x%llx\n",
			ctx->indent, (u64)cl);
	}
	return 0;
}

#define CORE_SERVER_DISK_STATUS_PROC_FRMT_VER 1
static ssize_t fill_disk_status(void *arg, char *buf, size_t len) {
	struct nvmeibs_disk_info *di = arg;
	struct nvmeibs_disk_private_data *priv = di->priv;
	ssize_t count = 0;
	int n_clients = 0;

	count += scnprintf(buf + count, len - count, "id: %s\n", di->disk_id);
	count += scnprintf(buf + count, len - count, "di_ptr: 0x%016llx\n", (u64)di);
	count += scnprintf(buf + count, len - count, "max-ioqs: %d, min/max-local-qs=%d/%d, max-client-qs=%d\n",
					   di->max_ioqs, di->min_local_ioqs, di->max_local_ioqs, di->n_qs);
	count += scnprintf(buf + count, len - count, "lock_dev:");
	if (!priv->lock_dev) {
		count += scnprintf(buf + count, len - count, " NULL\n");
	} else {
		count += scnprintf(buf + count, len - count, "\n\tname: %s\n",
			priv->lock_dev->dev->ib_dev->name);
		count += scnprintf(buf + count, len - count, "\tptr: 0x%016llx\n",
			(u64)priv->lock_dev);
	}
	if (get_nvme_dma_device(di->dev) != NULL) {
	#ifdef CONFIG_NUMA
		count += scnprintf(buf + count, len - count, "numa: %d\n", get_nvme_dma_device(di->dev)->numa_node);
		count += scnprintf(buf + count, len - count, "socket: %d\n", nvmeib_socket_from_numa(get_nvme_dma_device(di->dev)->numa_node));
	#endif
	}

	count += scnprintf(buf + count, len - count, "clients:\n");
	nvmeibs_cdb_all_fast_call(__fill_disk_client_status, &(struct __fill_disk_client_status_ctx){
		.buf = buf,
		.len = len,
		.count = &count,
		.n_clients = &n_clients,
		.di = di,
		.indent = "\t"
	});

	count += scnprintf(buf + count, len - count, "n_clients: %d\n", n_clients);

	count += scnprintf(buf + count, len - count, "stats:\n");
	count += scnprintf(buf + count, len - count, "\ttotal_ops: %llu\n", di->stats.total_ops);
	count += scnprintf(buf + count, len - count, "\ttotal_err: %llu\n", di->stats.total_err);
	count += scnprintf(buf + count, len - count, "\terr_rate: %llu #(percent)\n", di->stats.total_ops ? di->stats.total_err * 100 / di->stats.total_ops: 0);
#define RECORD_STAT_OP(op_) \
	count += scnprintf(buf + count, len - count, "\t"#op_":\n"); \
	count += scnprintf(buf + count, len - count, "\t\ttotal_ops: %llu\n", di->stats.op_.total_ops); \
	count += scnprintf(buf + count, len - count, "\t\ttotal_lat: %llu #(cpu_cycles)\n", di->stats.op_.total_lat); \
	count += scnprintf(buf + count, len - count, "\t\tmax_lat: %llu #(ns)\n", di->stats.op_.max_lat * 1000000 / nvmeib_public_tsc_khz()); \
	count += scnprintf(buf + count, len - count, "\t\tavg_lat: %llu #(ns)\n", di->stats.op_.total_ops ? di->stats.op_.total_lat / di->stats.op_.total_ops * 1000000 / nvmeib_public_tsc_khz() : 0);

	RECORD_STAT_OP(read);
	RECORD_STAT_OP(write);
	RECORD_STAT_OP(discard);
#undef RECORD_STAT_OP

	count += nvmeib_proc_add_yaml_proc_epilog(CORE_SERVER_DISK_STATUS_PROC_FRMT_VER, buf + count, len - count);
	return count;
}

static ssize_t fill_disk_qps(void *arg, char *buf, size_t len)
{
	struct nvmeibs_disk_info *di = arg;
	ssize_t count = 0;

	count += nvmeibs_nvme_disk_qp_stats_fill(di, buf, len);

	return count;
}

static ssize_t reset_disk_qps(void *arg, char *buf, size_t len)
{
	nvmeibs_nvme_disk_qp_stats_reset((struct nvmeibs_disk_info *)arg);
	return len;
}

static void destroy_disk_procfs(struct nvmeibs_disk_info *di)
{
	struct nvmeibs_disk_private_data *priv = di->priv;

	_NT(t0_destroy_disk_procfs, "Remove procfs of disk @DISK_NAME", di->disk_id);

	if (priv->procfs.status) {
		_NT(t1_destroy_disk_procfs, "Remove file status");
		nvmeib_public_proc_remove(priv->procfs.status);
		priv->procfs.status = NULL;
	}
	if (priv->procfs.qps) {
		_NT(t2_destroy_disk_procfs, "Remove file qps");
		nvmeib_public_proc_remove(priv->procfs.qps);
		priv->procfs.qps = NULL;
	}
	if (priv->procfs.iostats) {
		_NT(destroy_disk_procfs_iostats, "Remove iostats proc");
		nvmeib_public_proc_remove(priv->procfs.iostats);
		priv->procfs.iostats = NULL;
	}
	if (priv->procfs.nvme_qp_stats) {
		_NT(destroy_disk_procfs_nvme_qpstats, "Remove nvme_qp stats proc");
		nvmeib_public_proc_remove(priv->procfs.nvme_qp_stats);
		priv->procfs.nvme_qp_stats = NULL;
	}
	if (priv->procfs.dir) {
		_NT(t3_destroy_disk_procfs, "Remove dir");
		remove_proc_entry(di->disk_id, nvmeibs_proc_disks_dir);
		priv->procfs.dir = NULL;
	}
}

static ssize_t stats_nvme_qps(void *priv, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	struct nvmeibs_disk_info *di = priv;

	if (!di)
		return 0;

	return nvmeibs_nvme_fill_stats_nvme_qps(di, buf, len);
#undef BUF_ADD
}

#define CORE_SERVER_IOSTATS_PROC_FRMT_VER 2 /* Bumped to 2 due to fix for [NVMESH-6726] */
static ssize_t stats_fill_buf_json(void *priv, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibs_disk_info *di = priv;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	unsigned long disk_uptime = jiffies - di->add_jif;
	NFIN;
	count += jops->start_obj(buf + count, len - count, NULL, indent++);
	count += nvmeib_io_stats_to_json(di->io_stats, buf+count, len-count, disk_uptime, jops, indent, true);
	count += nvmeib_proc_add_json_proc_epilog(CORE_SERVER_IOSTATS_PROC_FRMT_VER, buf + count, len - count);
	count += jops->end_obj(buf + count, len - count, JSON_LAST_ELEM, --indent);
	NFOUT;
	return count;
#undef BUF_ADD
}

static ssize_t stats_clear(void *priv, char *buf , size_t len) {
	const struct nvmeibs_disk_info *di = priv;
	int reset;

	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		return -EINVAL;
	}

	nvmeib_io_stats_clear(di->io_stats , 'A');
	return len;
}

static int create_disk_procfs(struct nvmeibs_disk_info *di) {
	int rv;
	struct nvmeibs_disk_private_data *priv = di->priv;
	NFIN;

	BUG_ON(!priv);
	WARN_ON(priv->procfs.dir || priv->procfs.status || priv->procfs.qps);

	_NT(t0_create_disk_procfs, "Create dir /proc/nvmeibs/disks/@DISK_NAME", di->disk_id);
	if (!(priv->procfs.dir = proc_mkdir(di->disk_id, nvmeibs_proc_disks_dir))) {
		rv = -EEXIST;
		goto err;
	}

	_NT(t1_create_disk_procfs, "Create file /proc/nvmeibs/disks/@DISK_NAME/status", di->disk_id);
	if (!(priv->procfs.status = nvmeib_public_proc_create("status", priv->procfs.dir,
	                                             &fill_disk_status, NULL, di))) {
		rv = -EEXIST;
		goto err;
	}

	_NT(t2_create_disk_procfs, "Create file /proc/nvmeibs/disks/@DISK_NAME/qps", di->disk_id);
	if (!(priv->procfs.qps = nvmeib_public_proc_create("qps", priv->procfs.dir,
												 &fill_disk_qps, &reset_disk_qps, di))) {
		rv = -EEXIST;
		goto err;
	}

	_NT(create_disk_procfs_iostats, "Create file /proc/nvmeibs/disks/@DISK_NAME/iostats.json", di->disk_id);
	if (!(priv->procfs.iostats = nvmeib_public_proc_create("iostats.json", priv->procfs.dir,
	                                             &stats_fill_buf_json, &stats_clear, di))) {
		rv = -EEXIST;
		goto err;
	}

	if (!di->external) {
		_NT(create_disk_procfs_nvme_qps, "Create file /proc/nvmeibs/disks/@DISK_NAME/nvme_qps", di->disk_id);
		if (!(priv->procfs.nvme_qp_stats = nvmeib_public_proc_create("nvme_qps", priv->procfs.dir,
													&stats_nvme_qps, NULL, di))) {
			rv = -EEXIST;
			goto err;
		}
	}

	rv = 0;
	goto out;
err:
	_NE(create_disk_procfs_err, "Error creating procfs for disk @DISK_NAME rv=@RV", di->disk_id, rv);
	destroy_disk_procfs(di);
out:
	NFOUT;
	return rv;
}

struct proc_dir_entry *get_disk_proc_dir(struct nvmeibs_disk_info *di)
{
	struct proc_dir_entry *proc_dir = ERR_PTR(-ENOENT);
	struct nvmeibs_disk_private_data *priv = di->priv;
	if (priv->procfs.dir)
		proc_dir = priv->procfs.dir;
	return proc_dir;
}

static int create_disk(struct nvmeibs_disk_info *info)
{
	struct nvmeibs_disk_private_data *disk_private_data = NULL;
	struct disk_ *disk = NULL;
	struct rsc_info_ *rscs = NULL;
	struct timeval tv;
	char name_buf[64];
	int i;
	u64 num_of_vol_blocks;
	int rv = -1;

	NFIN;

	_NT(trace_disk_create_disk, "DISKLOCK: have only @BLOCKS blocks", info->blocks);
	disk = kzalloc(sizeof(*disk), GFP_KERNEL);
	rscs = kzalloc(sizeof(*rscs) * info->n_qs, GFP_KERNEL);
	if (!(disk && (!info->n_qs || rscs))) {
		_NE(error_disk_create_disk, " OOM: fail to allocate disk resources");
		goto err;
	}
	memcpy(disk->disk_id, info->disk_id, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	disk->vendor_id = nvmeibs_nvme_get_vendor(info);
	/* make sure the disk is alive at this point */
	info->dying = false;
	disk->info = info;
	disk->rscs = rscs;
	disk->clients_with = RB_ROOT;
	INIT_LIST_HEAD(&disk->rm_link);
	disk_private_data = &disk->locks_priv;
	INIT_LIST_HEAD(&disk_private_data->lock_mems);
	disk_private_data->n_memsegs = 0;
	do_gettimeofday(&tv);
	atomic64_set(&disk_private_data->disk_lock_counter, tv.tv_sec << 32);
	_NT(trace_1_disk_create_disk, "DISKLOCK: setting disk_lock_counter to @LOCK_COUNTER",
		(u64)atomic64_read(&disk_private_data->disk_lock_counter));
	disk_private_data->lock_dev_refcnt = 0;
	INIT_LIST_HEAD(&disk->prefered_ports);
	_NT(trace_2_disk_create_disk, "initialized prefered_ports of disk @DISK_ID_STR", disk->disk_id);
	INIT_LIST_HEAD(&disk->clients_wait);
	INIT_LIST_HEAD(&disk->avail_rsc);
	_NT(trace_3_disk_create_disk, "Got @N_QS qs", info->n_qs);
	for (i = 0; i < info->n_qs; ++i) {
		disk->rscs[i].qs = &info->qs[i];
		list_add_tail(&disk->rscs[i].link, &disk->avail_rsc);
		++disk->n_a;
	}

	/*register the whole disk as one lock segment*/
	_ND(trace_4_disk_create_disk, "LOCKS: whole disk allocation for disk @DISK_ID_STR, num of blocks @BLOCKS",
		info->disk_id, info->blocks);
	info->priv = disk_private_data;
	/*need to take into account the ratio between physical blocks and volume
	  block size.*/
	/*ensure that block size is power of two*/
	if (info->block_size == 0 || (info->block_size & (info->block_size - 1))) {
		_NE(error_1_disk_create_disk, "block size is NOT power of 2, @BLOCK_SIZE", info->block_size);
		goto err;
	}
	if (info->block_size <= 4096)
		num_of_vol_blocks = info->blocks / (4096 / info->block_size);
	else
		num_of_vol_blocks = info->blocks * (info->block_size / 4096);
	_ND(trace_5_disk_create_disk, "LOCKS: disk block size = @BLOCK_SIZE disk blocks @BLOCKS vol blocks = @NUM_OF_VOL_BLOCKS",
		info->block_size, info->blocks, num_of_vol_blocks);
	if (nvmeibs_disk_alloc_lock_memory(info, NVMEIBS_DEFAULT_DISK_SEG_ID, 0,
			NVMEIBS_DEFAULT_LOCK_SET_SIZE, num_of_vol_blocks, nvmeib_lock_init_value.all)) {
		_NE(error_2_disk_create_disk, "segment lock allocation ended with error");
		goto err;
	}
	if ((rv = create_disk_procfs(info))) goto err;
	/* create proc file for memory map lock-table to user space */
	snprintf(name_buf, sizeof name_buf, "locks.%s", info->disk_id);
	disk_private_data->proc_locks = nvmeib_public_mmap_create(
		name_buf, nvmeibs_proc_dir,
		&nvmeibs_disk_mmap_fault, disk_private_data,
		&locks_proc_on_last_mmap, NULL, NULL, NULL, 0400);
	if (disk_private_data->proc_locks == NULL) {
		_NE(error_3_disk_create_disk, "Cannot create locks /proc file");
		goto err;
	}
	atomic_inc(&n_mmaps);

	if (info->metadata) {
		/* Disk has metadata: Initialize Serjio for Disk */
		if (nvmeibs_serjio_disk_init(info)) {
			_NE(error_disk_create_disk_serjio_init_fail, 
			    "SERJIO: Failed Initialize SERJIO for disk @DISK_ID_STR", info->disk_id);
		}
	}

	nvmeibs_um_comm_add_disk(nvmeibs_get_um_comm(), info);
	rv = 0;
	goto out;

err:
	destroy_disk_procfs(info);
	kfree(rscs);
	kfree(disk);
	info->priv = NULL;

out:
	NFOUT;
	return rv;
}

static int add_disk_(struct nvmeibs_disk_info *info)
{
	int rv;
	struct nvmeibs_disk_info *di_iter;
	NFIN;

	if (!(rv = create_disk(info))) {
		nvmeib_ref_init(&info->nref);
		nvmeib_ref_init(&info->controller_ops_ref);
		list_add_tail(&info->link, &disk_info_list);
		++disk_count;

#if 0 /* [NVMESH-4781]: Moved to create_disk() */
		if (info->metadata) {
			/* Disk has metadata: Initialize Serjio for Disk */
			if (nvmeibs_serjio_disk_init(info)) {
				_NE(error_disk_add_disk, "SERJIO: Failed Initialize SERJIO for disk @DISK_ID_STR", info->disk_id);
			}
		}
#endif
		if (nvmeibs_disk_scan_finished()) {
			nvmeibs_register_disk_resources_at_all_nics(info);
			nvmeibs_toma_report_event_disk_change(info->disk_id, info->blocks, info->block_size,
												  info->max_request_size, info->seq, info->nsid,
												  info->gendisk->disk_name, info->metadata,
												  nvmeibs_get_status(info),
												  nvmeibs_nvme_get_vendor(info), 'a', nvmeibs_nvme_get_model(info->dev),
												  nvmeibs_nvme_get_native_serial(info->dev));

			/* Report all SERJIO status to TOMA */
			list_for_each_entry(di_iter, &disk_info_list, link) {
				nvmeibs_toma_report_event_serjio_state_change(
					di_iter->disk_id, nvmeibs_nvme_get_vendor(di_iter), nvmeibs_nvme_get_model(di_iter->dev),
					nvmeibs_serjio_get_status(di_iter));
			}
		}
		/* else
		   disk added prior to scan-done, register to NIC from do-add-one */
	}

	NFOUT;
	return rv;
}

int nvmeibs_disk_nvme_add_disk(struct nvmeibs_disk_info *info)
{
	struct disk_ *disk;
	bool found = false;
	int rv;

	NFIN;
	_NT(trace_disk_nvmeibs_disk_nvme_add_disk, "disk @DISK_ID_STR add - start", info->disk_id);
	nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(disk, &disk_rm_list, rm_link) {
		if (!memcmp(disk->disk_id, info->disk_id,
				NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			found = true;
			break;
		}
	}
	if (unlikely(found)) {
		_NT(trace_1_disk_nvmeibs_disk_nvme_add_disk, "disk @DISK_ID_STR: curr info-instance (@INFO_PTR) of disk is being removed, "
			"defer new-info add (new-info: curr @NEW_INFO, new @INFO_PTR)",
			disk->disk_id, disk->info, disk->new_info, info);
		/* let curr-rm completion know it shall add new instance */
		disk->new_info = info;
		rv = 0;
	}
	else if (unlikely(!list_empty(&info->link))) {
		_NE(error_disk_nvmeibs_disk_nvme_add_disk, "Disk @DISK_ID_STR already exists", info->disk_id);
		rv = -1;
	} else {
		rv = add_disk_(info);
	}
	nvmeibs_disk_put_disks();
	_NT(trace_2_disk_nvmeibs_disk_nvme_add_disk, "disk @DISK_ID_STR add - @STATUS_STR", info->disk_id,
		!found ? (!rv ? "Done" : "Failed") : "Deferred");
	NFOUT;
	return rv;
}

/*
 * Disk's locks proc file removal
 */
static void locks_proc_remove_(struct nvmeibs_disk_private_data *disk_pd)
{
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);
	bool disk_rm = !list_empty(&disk->rm_link);
	struct nvmeibs_disk_info *new_info;

	NFIN;
	nvmeib_public_mmap_remove(disk_pd->proc_locks);
	disk_pd->proc_rm.in_progress = false;
	disk_pd->proc_locks = NULL;
	atomic_dec(&n_mmaps);
	_NT(trace_disk_locks_proc_remove, "disk-pd @DISK_PD, locks proc removed", disk_pd);

	/* handle actions waiting for proc removal completion */
	if (disk_rm || disk->free_locks_mem) {
		if (disk_pd->n_memsegs > 0) {
			nvmeibs_disk_free_lock_memory(disk_pd);
			disk->free_locks_mem = false;
		}
	}

	if (disk_rm) {
		new_info = disk->new_info;
		list_del_init(&disk->rm_link);
		free_disk(disk);

		if (new_info) {
			_NT(trace_1_disk_locks_proc_remove, "Adding deferred instance of disk @DISK_ID_STR", new_info->disk_id);
			new_info->priv = NULL;
			if (add_disk_(new_info) < 0) {
				_NE(error_disk_locks_proc_remove, "Failed to add deferred instance of disk @DISK_ID_STR",
					new_info->disk_id);
				BUG_ON(!list_empty(&new_info->link));
			}
		}
	}
	NFOUT;
}

static void locks_proc_remove_work(struct workqe_struct *work)
{
	struct proc_remove *proc_rm = container_of(work, struct proc_remove, work);
	struct nvmeibs_disk_private_data *disk_pd =
		container_of(proc_rm, struct nvmeibs_disk_private_data, proc_rm);
	struct disk_ *disk = container_of(disk_pd, struct disk_, locks_priv);
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];

	NFIN;
	BUG_ON(!disk_pd->proc_locks);
	memcpy(disk_id, disk->disk_id, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	nvmeibs_disk_get_disks(NULL);
	locks_proc_remove_(disk_pd);
	nvmeibs_disk_put_disks();
	nvmeibs_remove_done(disk_id);
	NFOUT;
}

void locks_proc_on_last_mmap(void *arg)
{
	struct nvmeibs_disk_private_data *disk_pd = arg;
	int rv;

	NFIN;
	/* cant remove file here as:
	   (a) context may be the file's own fops method (.close)
	   (b) context may be interrupt and work may sleep (mutex) */
	_NT(trace_disk_locks_proc_on_last_mmap, "disk-pd @DISK_PD - defer proc locks remove", disk_pd);
	WQ_INIT_WORK(&disk_pd->proc_rm.work, locks_proc_remove_work);
	if ((rv = nvmeibs_add_work(&disk_pd->proc_rm.work)) < 0)
		_NE(error_disk_locks_proc_on_last_mmap, "Fail to add proc locks remove work (rv @RV)", rv);

	NFOUT;
}

static void locks_proc_remove_start_(struct nvmeibs_disk_private_data *disk_pd)
{
	struct proc_remove *proc_rm = &disk_pd->proc_rm;

	NFIN;
	BUG_ON(!disk_pd->proc_locks);
	if (proc_rm->in_progress)
		_NT(trace_disk_locks_proc_remove_start, "disk-pd @DISK_PD, proc remove in-progress not done", disk_pd);
	else {
		proc_rm->in_progress = true;
		nvmeib_public_mmap_release(disk_pd->proc_locks);
		/* when release is done, removal work is scheduled */
	}
	NFOUT;
}

static void remove_disk(struct nvmeibs_disk_info *info)
{
	struct nvmeibs_disk_private_data *disk_pd;
	struct disk_ *disk;
	int rv;

	NFIN;
	_NT(trace_disk_remove_disk, "disk @DISK_ID_STR (pd=@PRIV) -->", info->disk_id, info->priv);
	nvmeibs_um_comm_remove_disk(nvmeibs_get_um_comm(), info);

	/* Originally done after releasing all clients and removing @info from list.
	   This shall allow Toma to quickly update topology with this disk as N/A */
	nvmeibs_toma_report_event_disk_change(info->disk_id, info->blocks, info->block_size,
										  info->max_request_size, info->seq, info->nsid,
										  info->gendisk->disk_name, info->metadata,
										  nvmeibs_get_status(info),  nvmeibs_nvme_get_vendor(info), 'r', nvmeibs_nvme_get_model(info->dev),
										  nvmeibs_nvme_get_native_serial(info->dev));

	/* release all disk's clients on main-wq */
	while ((rv = nvmeibs_release_disk_clients(info, NVMEIBS_LOGOUT_REASON_REMOVE_DISK)) < 0)
		_NE(error_disk_remove_disk, "Fail to release all disk @DISK_ID_STR clients (rv @RV)", info->disk_id, rv);

	nvmeib_ref_release_start(&info->controller_ops_ref);
	nvmeib_ref_release_wait(&info->controller_ops_ref);

	nvmeibs_disk_get_disks(NULL);

	if (info->priv) {
		/* remove from info list */
		list_del_init(&info->link);
		--disk_count;
		_NT(trace_1_disk_remove_disk, "disk_count @DISK_COUNT", disk_count);

		/* reset links with nvme-layer */
		disk_pd = info->priv;
		disk = container_of(disk_pd, struct disk_, locks_priv);
		disk->info = NULL;
		BUG_ON(list_empty(&disk->rm_link));

		if (info->metadata)
			nvmeibs_serjio_disk_free(info);

		destroy_disk_procfs(info);

		/* defer removal of disk's proc-locks,
		   if any, until proc-locks file unmap */
		if (disk_pd->proc_locks) {
			locks_proc_remove_start_(disk_pd);
		} else {
			list_del_init(&disk->rm_link);
			if (disk_pd->n_memsegs > 0)
				nvmeibs_disk_free_lock_memory(disk_pd);
			free_disk(disk);
		}
	} else {
		_NE(error_1_disk_remove_disk, "Unexpected: disk just del from info-list but info->priv in NULL");
		BUG();
		//list_del_init(&disk->rm_link);
	}
	nvmeibs_disk_put_disks();

	_NT(trace_2_disk_remove_disk, "disk @DISK_ID_STR (pd=@PRIV) <--", info->disk_id, info->priv);
	NFOUT;
}

/**
 * This function may be called before or after nvmeibs-exit was done,
 * i.e. either 'Hot' or 'Cold' disk-remove, correspondingly.
 *
 * The disk object is only freed after its proc-locks file has no more
 * references (mmaps). In case of Cold remove, this ought to occur now
 * (in this function) as nvmeibs-exit already freed all locks resources
 * In case of Hot remove the disk obj free will happen asynchronously
 * and will block nvmeibs-exit as it executes on the srv's main-wq.
 */
void nvmeibs_disk_nvme_remove_disk(struct nvmeibs_disk_info *info)
{
	struct nvmeibs_disk_private_data *disk_pd;
	struct disk_ *disk;
	bool found = false;

	NFIN;
	_NT(trace_disk_nvmeibs_disk_nvme_remove_disk, "disk @DISK_ID_STR -->", info->disk_id);

	nvmeibs_disk_get_disks(NULL);
	if (!list_empty(&info->link)) {
		/* add disk to remove-list.
		   This lets new 'add' calls know that disk is being removed */
		disk_pd = info->priv;
		disk = container_of(disk_pd, struct disk_, locks_priv);
		list_add_tail(&disk->rm_link, &disk_rm_list);
		nvmeibs_disk_put_disks();
		/* client_q_reset_() can sneak-in here (from cl-wq ctx or other).
		   This is allowed, although delaying, while @info is valid i.e.
		   under s_disks' guard and before this func returns to nvme-layer */

		/* release all disk's clients, only
		   then remove disk from info-list */
		remove_disk(info);
	} else {
		list_for_each_entry(disk, &disk_rm_list, rm_link) {
			if (!memcmp(disk->disk_id, info->disk_id,
					NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
				found = true;
				break;
			}
		}
		if (found) {
			_NT(trace_1_disk_nvmeibs_disk_nvme_remove_disk, "disk @DISK_ID_STR: prev-removal NOT completed, info instance (@INFO_PTR) "
				"was not added yet", info->disk_id, info);
			disk->new_info = NULL; /* no new instance waiting for addition */
		} else {
			_NT(trace_2_disk_nvmeibs_disk_nvme_remove_disk, "disk @DISK_ID_STR: prev-removal completed but add of new info-instance "
				"had failed", info->disk_id);
			/* or, nvme-layer removes disk again after its prev (deferred)
			   rm completed */
		}
		nvmeibs_disk_put_disks();
	}

	_NT(trace_3_disk_nvmeibs_disk_nvme_remove_disk, "disk @DISK_ID_STR <--", info->disk_id);

	NFOUT;
}

void nvmeibs_disk_all_free_lock_resources(void)
{
	struct list_head *disks;
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *disk_pd;
	struct disk_ *disk;

	NFIN;
	disks = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, disks, link) {
		disk_pd = di->priv;
		if (disk_pd) {
			nvmeibs_toma_report_event_disk_change(di->disk_id, di->blocks, di->block_size, di->max_request_size,
												  di->seq, di->nsid, di->gendisk->disk_name,
												  di->metadata, nvmeibs_get_status(di), nvmeibs_nvme_get_vendor(di), 'r', nvmeibs_nvme_get_model(di->dev),
												  nvmeibs_nvme_get_native_serial(di->dev));
			disk = container_of(disk_pd, struct disk_, locks_priv);
			if (disk_pd->proc_locks) {
				disk->free_locks_mem = true;
				locks_proc_remove_start_(disk_pd);
			} else {
				if (disk_pd->n_memsegs > 0)
					nvmeibs_disk_free_lock_memory(disk_pd);
			}
		}
	}
	nvmeibs_disk_put_disks();
	_ND(trace_disk_nvmeibs_disk_all_free_lock_resources, "srv has @ATOMIC_READ locks mmap proc file(s) open", atomic_read(&n_mmaps));
	NFOUT;
}

static void print_clients(struct disk_ *disk)
{
	struct rb_node *node;
	struct client_info_ *ci;

	NFIN;

	/* YR: TODO: make this information accessible via /proc */

	_NT(trace_disk_print_clients, "Disk @DISK_ID_STR", disk->info->disk_id);
	for (node = rb_first(&disk->clients_with); node; node = rb_next(node)) {
		ci = rb_entry(node, struct client_info_, node);
		_ND(trace_1_disk_print_clients, "tree: @CL_NAME wi=@N_RSC, r=@R_RSC", ci->cl->name, ci->n_rsc, ci->r_rsc);
	}
	list_for_each_entry(ci, &disk->clients_wait, link)
	_ND(trace_2_disk_print_clients, "wait: @CL_NAME wa=@W_RSC", ci->cl->name, ci->w_rsc);
	NFOUT;
}

static void update_clients(struct disk_ *disk, int per_cl, int pref)
{
	struct rb_root dummy = RB_ROOT;
	struct rb_node *node;
	struct client_info_ *ci;

	NFIN;
	/* update the client with */
	while ((node = rb_first(&disk->clients_with))) {
		ci = rb_entry(node, struct client_info_, node);
		rb_erase(&ci->node, &disk->clients_with);
		ci->r_rsc = ci->n_rsc - per_cl;
		//OFER:
		//ci->r_rsc may become negative for clinet w/ 0 resources;
		//when we do ++ci->n_rsc; later on, we might get to 0 -->
		//client w/ rsc that we think it has no resources
		if (pref-- > 0)
			--ci->r_rsc;
		ci_rb_insert(&dummy, ci, 1);
	}
	disk->clients_with = dummy;
	/* update each of the waiting clients with a new disk resource number
	   that are going to be allocated
	*/
	list_for_each_entry(ci, &disk->clients_wait, link) {
		ci->w_rsc = per_cl;
		if (pref-- > 0)
			++ci->w_rsc;
	}
	print_clients(disk);
	NFOUT;
}

static void get_from_clients(struct disk_ *disk, int per_cl)
{
	struct client_info_ *ci;
	struct rb_node *node;
	int need;

	NFIN;
	for (node = rb_first(&disk->clients_with); node; node = rb_next(node)) {
		ci = rb_entry(node, struct client_info_, node);
		/* check the number of requests needed */
		need = ci->r_rsc - ci->t_rsc;
		/* if the client has a resource surplus get it from it */
		if (need > 0) {
			/* update the current client */
			ci->t_rsc += need;
			/* get the resources from the client */
			nvmeibs_client_get_resource(ci->cl, disk->info->disk_id, need);
			//OFER:
			//if send fails, ci->t_rsc is not correct
			//srv still think it has sent extra <need> resource reclaims ...
		}
	}
	NFOUT;
}

static int client_q_reset_(struct nvmeibs_q_info *q)
{
	int rv = -1;
	ulong start_jif, dt;
	NFIN;

	start_jif = jiffies;
	rv = nvmeibs_nvme_client_q_reset(q);
	dt = jiffies - start_jif;
	if (dt > (2 * HZ))
		_NW(warn_disk_client_q_reset,
			"Disk @DISK_ID_STR, q-reset took @DURATION",
			q->disk->disk_id, dt);

	NFOUT;
	return rv;
}
static void nvmeibs_lock_disk_rsc_ci(struct disk_ *disk,
	struct client_info_ *ci, struct rsc_info_ *rsc)
{
	NFIN;

	//EC-5217: (Micron 7300)
	if (false && client_q_reset_(rsc->qs))
		_NT(trace_disk_nvmeibs_lock_disk_rsc_ci_0,
			"disk @DISK_ID_STR, reset q @QID failure",
			disk->disk_id, rsc->qs->qid);
	else
		_NT(trace_disk_nvmeibs_lock_disk_rsc_ci_2,
			"@DISK_ID_STR, Skip NVMe reset q @QID before handing to cl",
			disk->disk_id, rsc->qs->qid);

	/* save the client that uses rsc inside the resource */
	rsc->ci = ci;
	/* add the resource to the client */
	BUG_ON(!list_empty(&rsc->link));
	list_add_tail(&rsc->link, &ci->rsc);
	/* one more client resource */
	++ci->n_rsc;
	/* one less free resource */
	--disk->n_a;
	NFOUT;
}

static void wakeup_client(struct client_info_ *ci)
{
	struct disk_ *disk = ci->disk;
	struct rsc_info_ *rsc;
	__be64 *p = ci->locate_buf_p;
	__be64 *e = ci->locate_buf_end;
	int tot_rsc = ci->w_rsc;
	u64 i;

	NFIN;
	BUG_ON(ci->cl->is_local); //don't send resources to local client

	_NT(trace_0_disk_wakeup_client,
		"cl=@PTR, del from waiting-clients (dw=@WAIT, cw=@WAIT_FOR)",
		ci->cl, disk->n_wa, ci->w_rsc);
	list_del_init(&ci->link);
	--disk->n_wa;

	/* check how many resources can we send the client, if any */
	while (p + (tot_rsc + 1) > e)
		--tot_rsc;
	if (!tot_rsc) {
		_NW(warn_disk_wakeup_client,
			"Client buffer to small, cannot send any resource to it");
		goto out;
	}

	/* check we can still ref cl's msg-area */
	if (!nvmeib_ref_get(&ci->cl->msg_area_refcount)) {
		_NT(trace_disk_wakeup_client,
			"Cannot ref-get cl=@PTR msg-area, bail", ci->cl);
		goto out;
	}

	/* fill in the disk resource sinfo */
	*ci->locate_buf_p++ = cpu_to_be64(tot_rsc);
	while (tot_rsc--) {
		/* get the free available resource */
		rsc = list_first_entry(&disk->avail_rsc, struct rsc_info_, link);
		list_del_init(&rsc->link);
		/* get its index */
		i = ((void *)rsc - (void *)disk->rscs) / sizeof(*rsc);
		/* write the index into the message */
		*ci->locate_buf_p++ = cpu_to_be64(i);
		/* give the client the resource */
		nvmeibs_lock_disk_rsc_ci(disk, ci, rsc);
		/* one less waiting clients */
		--ci->w_rsc;
	}
	/* add client into disk sorted resource list*/
	ci_insert(disk, ci, 1);
	++disk->n_wi;
	/* wakeup the client */
	BUG_ON(!ci->cl->release_done);
	complete(ci->cl->release_done);
	nvmeib_ref_put(&ci->cl->msg_area_refcount);
out:
	NFOUT;
}

static void return_to_client(struct client_info_ *ci)
{
	NFIN;
	if (!nvmeib_ref_get(&ci->cl->msg_area_refcount)) {
		_NT(trace_return_to_client,
			"Cannot ref-get cl=@PTR msg-area, bail", ci->cl);
		goto out;
	}
	/* fill in the disk resource sinfo */
	*ci->locate_buf_p++ = cpu_to_be64(ci->w_rsc);
	/* wakeup the client */
	BUG_ON(!ci->cl->release_done);
	complete(ci->cl->release_done);
	nvmeib_ref_put(&ci->cl->msg_area_refcount);
out:
	NFOUT;
}

/* Give surplus resources to clients from 'with-resources' rb-tree,
   starting from the client with least resources. */
static void send_client(struct disk_ *disk)
{
	struct rb_root dummy = RB_ROOT;
	struct rb_node *node;
	struct client_info_ *ci;
	struct rsc_info_ *rsc;
	u64 i, n;

	NFIN;
	/* update the client with */
	while ((node = rb_first(&disk->clients_with))) {
		ci = rb_entry(node, struct client_info_, node);
		rb_erase(&ci->node, &disk->clients_with);
		if (ci->r_rsc < 0) {
			n = 0;
			while (disk->n_a && ci->r_rsc) {
				/* get a free resource */
				rsc = list_first_entry(&disk->avail_rsc, struct rsc_info_, link);
				list_del_init(&rsc->link);
				i = ((void *)rsc - (void *)disk->rscs) / sizeof(*rsc);
				/* give the client the resource */
				nvmeibs_lock_disk_rsc_ci(disk, ci, rsc);
				disk->ids[n++] = cpu_to_be64(i);
				/* one less resource to give the client */
				++ci->r_rsc;
			}
			/* give the client the resource */
			if (n) {
				_NT(trace_disk_send_client, "Sending @NUM_RESOURCES resources to client @CL_NAME", n, ci->cl->name);
				BUG_ON(ci->cl->is_local);
				nvmeibs_client_put_resource(ci->cl, disk->info->disk_id,
					disk->ids, n);
			}
		}
		ci_rb_insert(&dummy, ci, 1);
	}
	disk->clients_with = dummy;
}

static void check_available(struct disk_ *disk)
{
	struct client_info_ *ci;
	bool go = true;

	NFIN;
	_NT(trace_disk_check_available, "Check available");
	/* we have free resources so try to wakeup waiting clients */
	_ND(trace_1_disk_check_available, "start with: wait=@WAIT, avail=@AVAIL", disk->n_wa, disk->n_a);
	while (disk->n_wa && disk->n_a && go) {
		ci = list_first_entry(&disk->clients_wait, struct client_info_, link);
		_ND(trace_2_disk_check_available, "wait_for=@WAIT_FOR, avail=@AVAIL", ci->w_rsc, disk->n_a);
		if (ci->w_rsc && ci->w_rsc <= disk->n_a) {
			wakeup_client(ci);
			_ND(trace_3_disk_check_available, "cont with: wait=@WAIT, avail=@AVAIL", disk->n_wa, disk->n_a);
		} else
			go = false;
	}
	/* if we still have resources and no waiting client add resources to
	   current working clients but we must remember no to give more than
	   the MAX_DISK_RESOURCES_PER_CLIENT
	*/
	_ND(trace_4_disk_check_available, "start with: wait=@WAIT, with=@WITH, avail=@AVAIL",
		disk->n_wa, disk->n_wi, disk->n_a);
	if (!disk->n_wa)
		send_client(disk);
	NFOUT;
}

static int calc_client_n_rsc(struct disk_ *disk, int *perf_clients)
{
	int n_clients, per_client;

	NFIN;
	if (max_client_rsrc > NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT)
		max_client_rsrc = NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT;
	/* and the total number of clients using the disk */
	n_clients = disk->n_wi + disk->n_wa;
	/* and the resources per client are adjusted */
	per_client = disk->info->n_qs / n_clients;
	if (per_client >= max_client_rsrc) {
		per_client = max_client_rsrc;
		if (perf_clients)
			*perf_clients = 0;
	} else if (perf_clients)
		*perf_clients = disk->info->n_qs - per_client * n_clients;
	_ND(trace_disk_calc_client_n_rsc, "with=@WITH, wait=@WAIT, n_qs=@N_QS, per_client=@PER_CLIENT, perf_clients=@PERF_CLIENTS",
		disk->n_wi, disk->n_wa, disk->info->n_qs, per_client,
		perf_clients ? *perf_clients : -1);
	NFOUT;
	return per_client;
}

void nvmeibs_disk_nvme_ioq_alloc_done(struct nvmeibs_q_info *q)
{
	struct nvmeibs_disk_info *info =  NULL;
	struct nvmeibs_disk_private_data *disk_pd = NULL;
	struct disk_ *disk;
	struct rsc_info_ *rsc = NULL;
	int ii;
	int per_client, perf_clients;
	NFIN;

	nvmeibs_disk_get_disks(NULL);
	/* checks */
	if (!q || !(info = q->disk) || !(disk_pd = info->priv)) {
		_NE(error_disk_nvmeibs_disk_nvme_ioq_alloc_done, "Oops, q @QUEUE, disk-info @INFO_PTR, disk-pd @DISK_PD", q, info, disk_pd);
		goto unlock;
	}
	disk = container_of(disk_pd, struct disk_, locks_priv);
	if (!list_empty(&disk->rm_link)) {
		_NT(trace_disk_nvmeibs_disk_nvme_ioq_alloc_done, "disk @DISK_ID_STR (@DISK), srv's disk-remove was already called by nvme-layer",
			disk->disk_id, disk);
		WARN_ON(1);
		goto unlock;
	}
	/* loookup the q's resource */
	for (ii = 0; ii < info->n_qs; ++ii) {
		if (&info->qs[ii] == q) {
			if (info->qs[ii].qid == q->qid)
				rsc = &disk->rscs[ii];
			else
				_NE(error_1_disk_nvmeibs_disk_nvme_ioq_alloc_done, "qid conflict (@QID vs. @QID)", info->qs[ii].qid, q->qid);
			break;
		}
	}
	if (!rsc) {
		_NE(error_2_disk_nvmeibs_disk_nvme_ioq_alloc_done, "Fail to find disk resource of q @QUEUE, qid @QID", q, q->qid);
		goto unlock;
	}
	/* add q to available resources */
	if (list_empty(&rsc->link)) {
		rsc->qs = q;
		rsc->ci = NULL;
		list_add_tail(&rsc->link, &disk->avail_rsc);
		++disk->n_a;
	} else
		_NE(error_3_disk_nvmeibs_disk_nvme_ioq_alloc_done, "rsc @II (@RSC_PTR) already inuse", ii, rsc);
	/* find a client to use this resource */
	if (disk->n_wi + disk->n_wa) {
		per_client = calc_client_n_rsc(disk, &perf_clients);
		update_clients(disk, per_client, perf_clients);
		check_available(disk);
	}
unlock:
	nvmeibs_disk_put_disks();

	NFOUT;
}

#if 0
static int disk_rsc_free_(struct disk_ *disk, int rsc_id)
{
	struct rsc_info_ *rsc = &disk->rscs[rsc_id];
	int rv = -1;
	NFIN;

	if (!list_empty(&rsc->link)) {
		list_del_init(&rsc->link);
		--disk->n_a;
		if (!(rv = nvmeibs_nvme_ioq_free(disk->info->handle, rsc->qs->qid))) {
			rsc->qs = NULL;
			rsc->ci = NULL;
		} else
			_NE(error_disk_disk_rsc_free, "Fail to free disk rsc @RSC_ID (@RSC_PTR), leakage", rsc_id, rsc);
	} else
		_NE(error_1_disk_disk_rsc_free, "rsc @RSC_ID (@RSC_PTR) not inuse", rsc_id, rsc);

	NFOUT;
	return rv;
}

static int disk_rsc_alloc_(struct disk_ *disk)
{
	int rv;
	NFIN;

	if ((rv = nvmeibs_nvme_ioq_alloc_request(disk->info->handle, 1)) < 0)
		_NE(error_disk_disk_rsc_alloc, "Fail to alloc rsc disk @DISK_ID_STR (@DISK)", disk->disk_id, disk);

	NFOUT;
	return rv;
}
#endif

/* Triggered by client's discover procedure in which
   the client declares it is ready to consume io resources */
static int locate(struct client_info_ *ci)
{
	struct disk_ *disk = ci->disk;
	int per_client, perf_clients;
	int rv = 0;
	int n_disk_rsrcs = ci->cdisk ? ci->cdisk->n_disk_rsrc : disk->info->n_qs;

	NFIN;
	WARN_ON(ci->cl->is_local); //we don't give resources to local clients
	/* check that disk can share resources.  this means that the number
	   of clients that already use the disk is less than
	   the maximun disk resources
	*/
	WARN_ON(!ci->cdisk);
	if (disk->n_wi + disk->n_wa >= n_disk_rsrcs && max_client_rsrc > 0) {
		_NT(trace_disk_locate, "Disk has no free resources to share (disk n_qs=@N_QS, cdisk n_disk_rsrc=@N_RSC"
			"max_client_rsrc=@MAX_CLIENT_RSRC)", disk->info->n_qs,
			ci->cdisk ? ci->cdisk->n_disk_rsrc : -1, max_client_rsrc);
		/* add client into disk sorted resource list */
		ci->w_rsc = 0;
		ci_insert(disk, ci, 1);
		++disk->n_wi;
		return_to_client(ci);
		goto out;
	}

	/* we have a new client that waits for resources */
	list_add_tail(&ci->link, &disk->clients_wait);
	++disk->n_wa;
	/* and the total number of clients using the disk is up by 1 so the
	   the per_client is less
	*/
	per_client = calc_client_n_rsc(disk, &perf_clients);
	/* and the currently waiting client */
	update_clients(disk, per_client, perf_clients);
#if 0
	/* if current client is the only waiting client try to allocate
	   from available resources
	*/
	_ND(locate_d1, "wait=@INT", disk->n_wa);
	if (disk->n_wa == 1)
	check_available(disk);
#endif
	/* if we still have waiting clients get resources from running clients */
	_ND(trace_1_disk_locate, "wait=@WAIT", disk->n_wa);
	if (disk->n_wa) {
		get_from_clients(disk, per_client);
		check_available(disk);
	}
out:
	NFOUT;
	return rv;
}

static inline struct nvmeibs_disk_info *
nvmeibs_locate_disk_nolock(const char *disk_name)
{
	struct nvmeibs_disk_info *di;
	list_for_each_entry(di, &disk_info_list, link) {
		if (!memcmp(di->disk_id, disk_name, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			goto out;
		}
	}
	di = NULL;

out:
	return di;
}

/*
 * return values:
 * 1 - wait for resources
 * 0 - don't wait for resources
 */
int nvmeibs_disk_client_locate(struct nvmeibs_client *cl, const char *disk_name,
							   const char *cl_ofed_ver, const char *cl_kern_ver,
							   struct volume_server_config_locate_rsp *lrsp,
	void *p, void *e)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *dp;
	struct disk_ *disk;
	struct client_info_ *ci;
	int rv = 0;

	NFIN;
	/* [EXC-1798] Check for OFED/Kernel Version Compatibility, otherwise don't assign resources */
	lrsp->ofed_kern_mismatch = false;
	lrsp->no_ofed = strncmp(INBOX_OFED_VER_STRING, OFED_VER_STRING,
				NVMEIB_MAX_OFED_VER_STRLEN) == 0;
	strncpy(lrsp->kern_ver, KERN_VER_STRING, NVMEIB_MAX_KERN_VER_STRLEN);
	strncpy(lrsp->ofed_ver, OFED_VER_STRING, NVMEIB_MAX_OFED_VER_STRLEN);
	if (strncmp(cl_ofed_ver, OFED_VER_STRING, NVMEIB_MAX_OFED_VER_STRLEN) != 0) {
		_NT(trace_disk_nvmeibs_disk_client_locate, "Mismatch between Client OFED: @CL_OFED_VER and Target OFED: @OFED_VER_STRING. Not assigning resources",
		cl_ofed_ver, OFED_VER_STRING);
		lrsp->ofed_kern_mismatch = true;
	} else if (lrsp->no_ofed &&
			strncmp(cl_kern_ver, KERN_VER_STRING, NVMEIB_MAX_KERN_VER_STRLEN) != 0) {
		_NT(trace_1_disk_nvmeibs_disk_client_locate, "INBOX Driver: Mismatch between Client Kernel @CL_KERN_VER and Target Kernel @KERN_VER_STRING. "
			"Not assigning resources", cl_kern_ver, KERN_VER_STRING);
		lrsp->ofed_kern_mismatch = true;
	}
	nvmeibs_disk_get_disks(NULL);
	/* client needs disk resources so firstly check that we such disk */
	_ND(trace_2_disk_nvmeibs_disk_client_locate, "disk_name=@DISK_NAME", disk_name);
	di = nvmeibs_locate_disk_nolock(disk_name);
	if (di) {
		if (!lrsp->ofed_kern_mismatch && max_client_rsrc > 0) {
			/* the locate only happenes for new client we are safe to assume
			   that there is no such client and thus create it
			*/
			if ((ci = create_client(cl))) {
				struct nvmeibs_client_disk *cdisk_iter;
				dp = di->priv;
				disk = container_of(dp, struct disk_, locks_priv);
				/* This is just a precaution as client for which srv has not
				   reported disk resources to (e.g. TCP client), will not send
				   LOCATE_RSC req */
				list_for_each_entry(cdisk_iter, &cl->disks, link) {
					if (cdisk_iter->di == di) {
						ci->cdisk = cdisk_iter;
						break;
					}
				}
				ci->disk = disk;
				ci->locate_buf_start = ci->locate_buf_p = p;
				ci->locate_buf_end = e;
				/* we have a new client so now we try to locate resources for it */
				if (locate(ci)) {
					kfree(ci);
				} else {
					_ND(trace_3_disk_nvmeibs_disk_client_locate, "Adding client @CL_NAME (@CL) into disk @DISK_NAME",
						cl->name, cl, disk_name);
					print_clients(ci->disk);
					rv = 1;
				}
			}
		} else {
			nvmeibs_client_create_disk_cache(cl, &disk_info_list, disk_count);
		}
	}
	nvmeibs_disk_put_disks();
	NFOUT;
	return rv;
}

static void poison_rdda_q_bb(struct nvmeibs_q_info *q, u32 cid)
{
	int i;
	const u64 poison = NVMEIBS_RDDA_BB_POISON_MSBS_U64 | ((u64)cid << 32);
	u64 *u64_p = q->bb_addr_virt[0];
	int n = PAGE_SIZE / sizeof(*u64_p);
	NFIN;

	BUILD_BUG_ON(sizeof(*u64_p) != sizeof(poison));
	_NT(trace_poison_rdda_q_bb, "poison rdda bb @LLX", poison);

	for (i = 0; i < n; i++)
		memcpy(&u64_p[i], &poison, sizeof(*u64_p));

	for (i = 1; i < q->bb_npages; i++)
		memcpy(q->bb_addr_virt[i], q->bb_addr_virt[0], PAGE_SIZE);

	NFOUT;
}

int nvmeibs_disk_client_reset_io(struct nvmeibs_client *cl,
	const char *disk_name, u64 rsc_id,
	u64 msix_table_addr, u64 msix_raddr, u32 msix_payload)
{
	struct nvmeibs_disk_info *di;
	int i, rv = -ENOENT;
	bool ref_get_done = false;

	NFIN;

	/* q-reset may prolong (vendor dependent),
	   get refcnt to avoid reset under mutex */
	nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, &disk_info_list, link) {
		if (!memcmp(di->disk_id, disk_name,
				NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			if (nvmeib_ref_get(&di->controller_ops_ref)) {
				ref_get_done = true;
			}
			break;
		}
	}
	nvmeibs_disk_put_disks();

	if (ref_get_done) {
		for (i = 0; i < di->n_qs; ++i) {
			if (di->qs[i].qid == (int)rsc_id) {
				if (!(rv = client_q_reset_(&di->qs[i]))) {
					poison_rdda_q_bb(&di->qs[i], cl->cid);
					set_msix_vector(
						&di->qs[i], msix_table_addr, msix_raddr, msix_payload);
				}
				break;
			}
		}
		nvmeib_ref_put(&di->controller_ops_ref);
	}
	NFOUT;
	return rv;
}

static void remove_from_disk(struct disk_ *disk,
	struct nvmeibs_client *cl)
{
	struct client_info_ *ci = NULL;
	struct rb_node *node;
	struct rsc_info_ *r, *tr;
	int per_client, perf_clients;
	bool found = false;

	NFIN;
	_ND(trace_disk_remove_from_disk, "Removing client @CL_NAME from disk @DISK_ID_STR", cl->name, disk->info->disk_id);
	/* check if client has resources */
	for (node = rb_first(&disk->clients_with); node; node = rb_next(node)) {
		ci = rb_entry(node, struct client_info_, node);
		_ND(trace_1_disk_remove_from_disk, "ci->cl=@CL, cl=@CL", ci->cl, cl);
		if (ci->cl == cl) {
			_ND(trace_2_disk_remove_from_disk, "Found client @CL_NAME with resources on disk @DISK_ID_STR",
				cl->name, disk->info->disk_id);
			/* go through all its resources and release them */
			list_for_each_entry_safe(r, tr, &ci->rsc, link) {
				/* Remove resource from client */
				_NT(trace_3_disk_remove_from_disk, "Taking disk-rsc=@RSC from client @CL_NAME", r->qs->qid, cl->name);
				list_del_init(&r->link);
				mask_msix_vector(r->qs);
				r->ci = NULL;
				/* Return reset resource to disk */
				list_add_tail(&r->link, &disk->avail_rsc);
				++disk->n_a;
			}
			/* one less client using the disk */
			--disk->n_wi;
			rb_erase(&ci->node, &disk->clients_with);
			found = true;
			break;
		}
	}
	/* the client has no disk resources so it might waiting for some */
	if (!found) {
		list_for_each_entry(ci, &disk->clients_wait, link)
		if (ci->cl == cl) {
			_ND(trace_4_disk_remove_from_disk, "Found client @CL_NAME waiting on disk @DISK_ID_STR",
				cl->name, disk->info->disk_id);
			/* the client is waiting so stop it */
			list_del_init(&ci->link);
			--disk->n_wa;
			found = true;
			//OFER: Fairness issue
			//if a new clinet will get into waiting list and will preceed
			//other that is already in with-tree but with 0 rscs
			break;
		}
	}
	if (found) {
		/* if we still have disk clients then update the
		   per client for the waiting
		*/
		if (disk->n_wi + disk->n_wa) {
			per_client = calc_client_n_rsc(disk, &perf_clients);
			update_clients(disk, per_client, perf_clients);
			check_available(disk);
		}
		kfree(ci);
	}
	NFOUT;
}

static void make_resources_available(struct disk_ *disk,
	struct nvmeibs_client *cl, struct volume_client_get_rsp *rsp)
{
	struct client_info_ *ci;
	struct rb_node *node;
	int i;
	unsigned j;
	int n;

	NFIN;
	for (node = rb_first(&disk->clients_with); node; node = rb_next(node)) {
		ci = rb_entry(node, struct client_info_, node);
		if (ci->cl == cl) {
			rb_erase(&ci->node, &disk->clients_with);
			n = (int)be64_to_cpu(rsp->n);
			_NT(trace_disk_make_resources_available, "Client @CL_NAME made available @N_RSCS resources", ci->cl->name, n);
			for (i = 0; i < n; ++i) {
				j = (unsigned)be64_to_cpu(rsp->ids[i]);
				if (j >= disk->info->n_qs || !disk->rscs[j].ci) {
					_NE(error_disk_make_resources_available, "OOPS: j=@RSC_ID", j);
					BUG();
				}
				if (j < disk->info->n_qs && disk->rscs[j].ci->cl == cl) {
					/* remove resource from client */
					list_del_init(&disk->rscs[j].link);
					--ci->n_rsc;
					--ci->r_rsc;
					--ci->t_rsc;
					/* add back resource to disk */
					disk->rscs[j].ci = NULL;
					list_add_tail(&disk->rscs[j].link, &disk->avail_rsc);
					++disk->n_a;
				}
			}
			if (ci->n_rsc < 0) {
				_NE(error_1_disk_make_resources_available, "OOPS: client with negative resources");
				ci->n_rsc = 0;
			}
			/* add client back to disk sorted resource list */
			ci_insert(disk, ci, 1);
			break;
		}
	}
	NFOUT;
}

void nvmeibs_disk_put_resources(struct nvmeibs_client *cl,
	const char *disk_name,  struct volume_client_get_rsp *rsp)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *dp;
	struct disk_ *disk;
	int rv = -1;

	NFIN;
	nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, &disk_info_list, link)
	if (!memcmp(di->disk_id, disk_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
		rv = 0;
		break;
	}
	if (!rv) {
		dp = di->priv;
		disk = container_of(dp, struct disk_, locks_priv);
		make_resources_available(disk, cl, rsp);
		check_available(disk);
		print_clients(disk);
	}
	nvmeibs_disk_put_disks();
	NFOUT;
}

void nvmeibs_disk_remove_client(struct nvmeibs_client *cl,
	const char *disk_name)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *dp;
	struct disk_ *disk;
	int rv = -1;

	NFIN;
	nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, &disk_info_list, link)
	if (!memcmp(di->disk_id, disk_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
		rv = 0;
		break;
	}
	if (!rv) {
		dp = di->priv;
		disk = container_of(dp, struct disk_, locks_priv);
		remove_from_disk(disk, cl);
		print_clients(disk);
	}
	//EC-5213
	#if 0
	if (cl->release_done) {
		kfree(cl->release_done);
		cl->release_done = NULL;
	}
	#endif
	nvmeibs_disk_put_disks();
	NFOUT;
}

void nvmeibs_disk_remove_client_all(struct nvmeibs_client *cl)
{
	struct nvmeibs_disk_info *di;
	struct nvmeibs_disk_private_data *dp;
	struct disk_ *disk;

	NFIN;
	nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, &disk_info_list, link) {
		dp = di->priv;
		disk = container_of(dp, struct disk_, locks_priv);
		print_clients(disk);
		remove_from_disk(disk, cl);
	}
	_ND(trace_disk_nvmeibs_disk_remove_client_all, "Printing remaining clients");
	list_for_each_entry(di, &disk_info_list, link) {
		dp = di->priv;
		disk = container_of(dp, struct disk_, locks_priv);
		print_clients(disk);
	}
	nvmeibs_disk_put_disks();
	NFOUT;
}

/*print disk clients information into buffer*/
ssize_t nvmeibs_disk_print_disk_res_info(struct nvmeibs_disk_info *di,
	char *buffer, int len)
{
	struct disk_ *disk;
	struct nvmeibs_disk_private_data *disk_pd;
	struct rb_node *node;
	struct client_info_ *ci;
	int count = 0;
	bool first = true;

	NFIN;
	disk_pd = (struct nvmeibs_disk_private_data *)di->priv;
	disk = container_of(disk_pd, struct disk_, locks_priv);
	count += scnprintf(buffer + count, len - count,
		"{ \"name\":\"%s\",\n \"num_qs\":%d,\n", di->disk_id, di->n_qs);
	count += nvmeibs_nvme_print_qs(di->dev, buffer + count, len - count);
	count += scnprintf(buffer + count, len - count, ",\n\"clients_with\": [\n");
	for (node = rb_first(&disk->clients_with); node; node = rb_next(node)) {
		if (!first)
			count += scnprintf(buffer + count, len - count, ",");
		first = false;
		ci = rb_entry(node, struct client_info_, node);
		count += scnprintf(buffer + count, len - count,
			"{\"name\":\"%s\",\n \"rsc\":%d,\n\"w_rcs\":%d,\n"
			"\"s_client\":",
			ci->cl->name, ci->n_rsc, ci->w_rsc);
		count += nvmeibs_client_print_client_info(ci->cl,
			buffer + count, len - count);
		count += scnprintf(buffer + count, len - count, "}\n");
	}
	first = true;
	count += scnprintf(buffer + count, len - count, "],\n");
	count += scnprintf(buffer + count, len - count,
		"\"clients_wait\": [\n");
	list_for_each_entry(ci, &disk->clients_wait, link) {
		if (!first)
			count += scnprintf(buffer + count, len - count, ",");
		first = false;
		count += scnprintf(buffer + count, len - count,
			"{\"name\":\"%s\",\n \"rsc\":%d,\"w_rcs\":%d"
			"\n,\n\"s_client\":",
			ci->cl->name, ci->n_rsc, ci->w_rsc);
		count += nvmeibs_client_print_client_info(ci->cl,
			buffer + count, len - count);
		count += scnprintf(buffer + count, len - count, "}\n");
	}
	count += scnprintf(buffer + count, len - count, "]}\n");
	NFOUT;
	return count;
}

void nvmeibs_disk_record_stats(struct nvmeibs_disk_info *di, struct nvmeibs_nvme_req *req, int status)
{
	if (!nvmeibs_disk_collect_stats)
		return;

#if NVMEIB_TRANSPORT_SKIP_STAGES
	if (req->skipped)
		return;
#endif

	if (status) { /* Record failure */
			di->stats.total_err++;
		} else { /* Record success */
			ktime_t end_time = nvmeib_public_ktime_get();
			ktime_t start_time = req->stats.start_time;
			u64 lat = ktime_after(end_time, start_time) ? ktime_to_ns(ktime_sub(end_time, start_time)) : 0;
			switch (req->nvme_op) {
				case nvme_cmd_write:
					di->stats.write.total_lat += lat;
					di->stats.write.max_lat = max(di->stats.write.max_lat, lat);
					di->stats.write.total_ops ++;
					nvmeib_io_stats_adjust_and_update(di->io_stats, NULL,
									  req->stats.is_recovery ? IO_STAT_VERB_RECOV_WRITE : IO_STAT_VERB_WRITE,
									  req->data_len, lat, false);
					if (req->nvme_qp_stats)
						req->nvme_qp_stats->wr_count++;
					break;
				case nvme_cmd_read:
					di->stats.read.total_lat += lat;
					di->stats.read.max_lat = max(di->stats.read.max_lat, lat);
					di->stats.read.total_ops ++;
					nvmeib_io_stats_adjust_and_update(di->io_stats, NULL,
									  req->stats.is_recovery ? IO_STAT_VERB_RECOV_READ : IO_STAT_VERB_READ,
									  req->data_len, lat, false);
					if (req->nvme_qp_stats)
						req->nvme_qp_stats->rd_count++;
					break;
				case nvme_cmd_dsm:
					di->stats.discard.total_lat += lat;
					di->stats.discard.max_lat = max(di->stats.discard.max_lat, lat);
					di->stats.discard.total_ops ++;
					nvmeib_io_stats_adjust_and_update(di->io_stats, NULL, IO_STAT_VERB_DISCARD,
									  req->n_dsm_lba << di->block_shift, lat, false);
					break;
				default:
					break;
			}
		}
		di->stats.total_ops ++;
}

#ifdef CONFIG_NUMA
int nvmeibs_disk_nic_numa_dist(struct nvmeibs_disk_info *disk,
	struct ib_device *ib_dev)
{
	int rv = 0;
	int disk_numa_node = 0, nic_numa_node = 0;

	NFIN;
	nic_numa_node = IBDEV2DMADEV(ib_dev)->numa_node;
	disk_numa_node = nvmeibs_disk_info_numa_node(disk);
	_NT(nvmeibs_disk_nic_numa_dist_t1, "disk numa node=@INT nic numa node=@INT", disk_numa_node, nic_numa_node);
	rv = (nic_numa_node == disk_numa_node) ?
		NVMEIBS_DISK_NIC_NUMA_SAME :
		NVMEIBS_DISK_NIC_NUMA_DIFF;

	NFOUT;
	return rv;
}
#else
int nvmeibs_disk_nic_numa_dist(struct nvmeibs_disk_info *disk,
	struct ib_device *nic_ib_dev)
{
	NFIN;
	_NT(trace_disk_nvmeibs_disk_nic_numa_dist, "No numa calculation");
	NFOUT;
	return NVMEIBS_DISK_NIC_NUMA_SAME;
}
#endif

